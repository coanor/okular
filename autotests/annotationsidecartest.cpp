/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QCryptographicHash>
#include <QFile>
#include <QMimeDatabase>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../core/annotations.h"
#include "../core/annotationsidecar_p.h"
#include "../core/document.h"
#include "../core/form.h"
#include "../core/page.h"
#include "../settings_core.h"

class AnnotationSidecarTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_storeDirectory;

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        qputenv("OKULAR_READING_DATA_PATH", m_storeDirectory.filePath(QUuid::createUuid().toString() + QStringLiteral(".sqlite")).toUtf8());
    }
    void cleanup()
    {
        qunsetenv("OKULAR_READING_DATA_PATH");
    }
    void replayAndQuery()
    {
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(QUuid::createUuid().toByteArray(), QCryptographicHash::Sha256).toHex());
        QString error;
        QList<Okular::SidecarAnnotation> loaded;
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QVERIFY(loaded.isEmpty());

        const Okular::SidecarAnnotation first {QStringLiteral("first"), 2, 4, QStringLiteral("<annotation id='first'/>")};
        const Okular::SidecarAnnotation second {QStringLiteral("second"), 4, 1, QStringLiteral("<annotation id='second'/>")};
        qint64 revision = 0;
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {first, second}, &error, 0, &revision), qPrintable(error));
        QCOMPARE(revision, 2);
        QVERIFY(!Okular::AnnotationSidecar::save(hash, {}, &error, 0));
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error, &revision), qPrintable(error));
        QCOMPARE(revision, 2);
        QCOMPARE(loaded.size(), 2);

        Okular::SidecarAnnotation changed = first;
        changed.xml = QStringLiteral("<annotation id='first' color='yellow'/>");
        changed.contents = QStringLiteral("Chapter note");
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {changed}, &error, revision, &revision), qPrintable(error));
        QCOMPARE(revision, 4);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().xml, changed.xml);
        QCOMPARE(loaded.first().contents, changed.contents);

        const QString connectionName = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(Okular::AnnotationSidecar::pathForHash(hash));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("SELECT annotation_id FROM annotations WHERE page = 2 AND subtype = 4 AND contents = 'Chapter note'")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), first.id);
            QVERIFY(!query.next());
            QVERIFY(query.exec(QStringLiteral("SELECT operation FROM annotation_events ORDER BY sequence")));
            QStringList operations;
            while (query.next()) {
                operations.append(query.value(0).toString());
            }
            QCOMPARE(operations, QStringList({QStringLiteral("upsert"), QStringLiteral("upsert"), QStringLiteral("delete"), QStringLiteral("upsert")}));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);

        const QString differentHash = QString::fromLatin1(QCryptographicHash::hash(QUuid::createUuid().toByteArray(), QCryptographicHash::Sha256).toHex());
        QVERIFY2(Okular::AnnotationSidecar::load(differentHash, &loaded, &error), qPrintable(error));
        QVERIFY(loaded.isEmpty());
        QCOMPARE(Okular::AnnotationSidecar::pathForHash(hash), Okular::AnnotationSidecar::pathForHash(differentHash));
        QVERIFY2(Okular::AnnotationSidecar::save(differentHash, {second}, &error, 0), qPrintable(error));
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().id, first.id);
    }

    void hashUsesPdfBytes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile first(dir.filePath(QStringLiteral("one.pdf")));
        QVERIFY(first.open(QIODevice::WriteOnly));
        first.write("same bytes");
        first.close();
        QFile second(dir.filePath(QStringLiteral("renamed.pdf")));
        QVERIFY(second.open(QIODevice::WriteOnly));
        second.write("same bytes");
        second.close();

        QString error;
        const QString hash = Okular::AnnotationSidecar::pdfHash(first.fileName(), &error);
        QCOMPARE(hash.size(), 64);
        QCOMPARE(hash, Okular::AnnotationSidecar::pdfHash(second.fileName(), &error));
        QVERIFY(second.open(QIODevice::Append));
        second.write(" changed");
        second.close();
        QVERIFY(hash != Okular::AnnotationSidecar::pdfHash(second.fileName(), &error));
    }

    void snapshotIncludesUncheckpointedWrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(QUuid::createUuid().toByteArray(), QCryptographicHash::Sha256).toHex());
        QString error;
        qint64 revision = 0;
        const QString sourcePath = Okular::AnnotationSidecar::pathForHash(hash);
        const Okular::SidecarAnnotation first {QStringLiteral("note"), 0, 1, QStringLiteral("<annotation/>")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {first}, &error, 0, &revision), qPrintable(error));

        const QString connectionName = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(sourcePath);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("wal"));
            query.finish();

            Okular::SidecarAnnotation updated = first;
            updated.xml = QStringLiteral("<annotation revised='yes'/>");
            QVERIFY(db.transaction());
            QVERIFY(query.exec(QStringLiteral("UPDATE annotations SET xml = '<annotation revised=''yes''/>' WHERE annotation_id = 'note'")));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO annotation_events(annotation_id, page, subtype, xml, operation, recorded_utc, book_hash, sequence) "
                                              "VALUES ('note', 0, 1, '<annotation revised=''yes''/>', 'upsert', '2026-01-01T00:00:00Z', '%1', 2)")
                                   .arg(hash)));
            QVERIFY(db.commit());
            ++revision;
            QVERIFY(QFile::exists(sourcePath + QStringLiteral("-wal")));
            const QString snapshotPath = dir.filePath(QStringLiteral("snapshot.sqlite"));
            qint64 snapshotRevision = -1;
            QVERIFY2(Okular::AnnotationSidecar::snapshot(hash, snapshotPath, &error, &snapshotRevision), qPrintable(error));
            QCOMPARE(snapshotRevision, revision);

            const QString snapshotConnectionName = QUuid::createUuid().toString();
            {
                QSqlDatabase snapshot = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), snapshotConnectionName);
                snapshot.setDatabaseName(snapshotPath);
                QVERIFY(snapshot.open());
                QSqlQuery snapshotQuery(snapshot);
                QVERIFY(snapshotQuery.exec(QStringLiteral("PRAGMA integrity_check")));
                QVERIFY(snapshotQuery.next());
                QCOMPARE(snapshotQuery.value(0).toString(), QStringLiteral("ok"));
                QVERIFY(snapshotQuery.exec(QStringLiteral("SELECT xml FROM annotations WHERE annotation_id = 'note'")));
                QVERIFY(snapshotQuery.next());
                QCOMPARE(snapshotQuery.value(0).toString(), updated.xml);
                snapshot.close();
            }
            QSqlDatabase::removeDatabase(snapshotConnectionName);
            QVERIFY(!Okular::AnnotationSidecar::snapshot(hash, snapshotPath, &error));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        QVERIFY(QFile::remove(sourcePath));
    }

    void pdfHighlightRoundTrip()
    {
        Okular::SettingsCore::instance(QStringLiteral("annotationsidecartest"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("book.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), pdfPath));
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::Append));
        pdf.write("\n% annotation-sidecar-test ");
        pdf.write(QUuid::createUuid().toString().toLatin1());
        pdf.write("\n");
        pdf.close();

        QString error;
        const QString hash = Okular::AnnotationSidecar::pdfHash(pdfPath, &error);
        const QMimeType mime = QMimeDatabase().mimeTypeForFile(pdfPath);
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(pdfPath, QUrl::fromLocalFile(pdfPath), mime), Okular::Document::OpenSuccess);

        auto *highlight = new Okular::HighlightAnnotation();
        const Okular::NormalizedRect rect(0.36, 0.16, 0.51, 0.17);
        highlight->setBoundingRectangle(rect);
        Okular::HighlightAnnotation::Quad quad;
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.bottom), 0);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.bottom), 1);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.top), 2);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.top), 3);
        highlight->highlightQuads().append(quad);
        document.addPageAnnotation(0, highlight);
        const QString id = highlight->uniqueName();
        QVERIFY(document.canSaveAnnotationsToSidecar());
        QVERIFY(document.hasSeparatePdfAnnotations());
        QVERIFY2(document.saveAnnotationsToSidecar(&error), qPrintable(error));
        QCOMPARE(Okular::AnnotationSidecar::pdfHash(pdfPath, &error), hash);
        document.closeDocument();

        const QString renamedPath = dir.filePath(QStringLiteral("same-book-renamed.pdf"));
        QVERIFY(QFile::copy(pdfPath, renamedPath));
        QCOMPARE(document.openDocument(renamedPath, QUrl::fromLocalFile(renamedPath), mime), Okular::Document::OpenSuccess);
        QVERIFY(document.page(0)->annotation(id));
        document.closeDocument();

        QCOMPARE(document.openDocument(pdfPath, QUrl::fromLocalFile(pdfPath), mime), Okular::Document::OpenSuccess);
        QVERIFY(document.hasSeparatePdfAnnotations());
        auto *restored = document.page(0)->annotation(id);
        QVERIFY(restored);
        QCOMPARE(restored->subType(), Okular::Annotation::AHighlight);
        document.removePageAnnotation(0, restored);
        QVERIFY(document.canSaveAnnotationsToSidecar());
        QVERIFY2(document.saveAnnotationsToSidecar(&error), qPrintable(error));
        document.closeDocument();

        QCOMPARE(document.openDocument(pdfPath, QUrl::fromLocalFile(pdfPath), mime), Okular::Document::OpenSuccess);
        QVERIFY(!document.page(0)->annotation(id));
        QVERIFY(!document.hasSeparatePdfAnnotations());
        QCOMPARE(Okular::AnnotationSidecar::pdfHash(pdfPath, &error), hash);
        document.closeDocument();
        QFile::remove(Okular::AnnotationSidecar::pathForHash(hash));
    }

    void formEditsPreventSidecarSave()
    {
        Okular::SettingsCore::instance(QStringLiteral("annotationsidecartest"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("forms.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/formSamples.pdf"), pdfPath));
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::Append));
        pdf.write("\n% form-sidecar-test ");
        pdf.write(QUuid::createUuid().toString().toLatin1());
        pdf.write("\n");
        pdf.close();

        const QMimeType mime = QMimeDatabase().mimeTypeForFile(pdfPath);
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(pdfPath, QUrl::fromLocalFile(pdfPath), mime), Okular::Document::OpenSuccess);
        Okular::FormFieldText *form = nullptr;
        for (Okular::FormField *field : document.page(0)->formFields()) {
            if (field->type() == Okular::FormField::FormText) {
                auto *textField = static_cast<Okular::FormFieldText *>(field);
                if (textField->textType() == Okular::FormFieldText::Normal) {
                    form = textField;
                    break;
                }
            }
        }
        QVERIFY(form);

        auto *note = new Okular::TextAnnotation();
        note->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.15, 0.15));
        document.addPageAnnotation(0, note);
        QVERIFY(document.canSaveAnnotationsToSidecar());

        document.editFormText(0, form, QStringLiteral("Hello"), 5, 0, 0, form->text());
        QCOMPARE(form->text(), QStringLiteral("Hello"));
        QVERIFY(!document.canSaveAnnotationsToSidecar());
        QString error;
        QVERIFY(!document.saveAnnotationsToSidecar(&error));
        QVERIFY(!error.isEmpty());
        document.closeDocument();
    }

    void fdPdfHighlightRoundTrip()
    {
#ifdef Q_OS_UNIX
        Okular::SettingsCore::instance(QStringLiteral("annotationsidecartest"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("android-open.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), pdfPath));
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::Append));
        pdf.write("\n% fd-sidecar-test ");
        pdf.write(QUuid::createUuid().toString().toLatin1());
        pdf.write("\n");
        pdf.close();

        auto openThroughFd = [&pdfPath](Okular::Document &document) {
            const int fd = ::open(QFile::encodeName(pdfPath).constData(), O_RDONLY);
            if (fd < 0) {
                return Okular::Document::OpenError;
            }
            return document.openDocument(QStringLiteral("-"), QUrl(QStringLiteral("fd:///%1").arg(fd)), QMimeType());
        };

        Okular::Document document(nullptr);
        QCOMPARE(openThroughFd(document), Okular::Document::OpenSuccess);
        QVERIFY(document.supportsAnnotationSidecar());
        auto *highlight = new Okular::HighlightAnnotation();
        const Okular::NormalizedRect rect(0.36, 0.16, 0.51, 0.17);
        highlight->setBoundingRectangle(rect);
        Okular::HighlightAnnotation::Quad quad;
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.bottom), 0);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.bottom), 1);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.top), 2);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.top), 3);
        highlight->highlightQuads().append(quad);
        document.addPageAnnotation(0, highlight);
        const QString id = highlight->uniqueName();
        QString error;
        QVERIFY2(document.saveAnnotationsToSidecar(&error), qPrintable(error));
        const QString hash = Okular::AnnotationSidecar::pdfHash(pdfPath, &error);
        document.closeDocument();

        QCOMPARE(openThroughFd(document), Okular::Document::OpenSuccess);
        QVERIFY(document.page(0)->annotation(id));
        QCOMPARE(Okular::AnnotationSidecar::pdfHash(pdfPath, &error), hash);
        document.closeDocument();
        QFile::remove(Okular::AnnotationSidecar::pathForHash(hash));
#endif
    }

    void embeddedAnnotationOverrideAndRemoval()
    {
        Okular::SettingsCore::instance(QStringLiteral("annotationsidecartest"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString originalPath = dir.filePath(QStringLiteral("original.pdf"));
        const QString embeddedPath = dir.filePath(QStringLiteral("embedded.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), originalPath));
        const QMimeType mime = QMimeDatabase().mimeTypeForFile(originalPath);
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(originalPath, QUrl::fromLocalFile(originalPath), mime), Okular::Document::OpenSuccess);
        auto *note = new Okular::TextAnnotation();
        note->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.15, 0.15));
        note->setContents(QStringLiteral("original note"));
        document.addPageAnnotation(0, note);
        const QString id = note->uniqueName();
        QString error;
        QVERIFY2(document.saveChanges(embeddedPath, &error), qPrintable(error));
        document.closeDocument();

        const QString hash = Okular::AnnotationSidecar::pdfHash(embeddedPath, &error);
        QCOMPARE(document.openDocument(embeddedPath, QUrl::fromLocalFile(embeddedPath), mime), Okular::Document::OpenSuccess);
        auto *embedded = document.page(0)->annotation(id);
        QVERIFY(embedded);
        QVERIFY(embedded->flags() & Okular::Annotation::External);
        QVERIFY(!document.hasSeparatePdfAnnotations());
        document.editPageAnnotationContents(0, embedded, QStringLiteral("updated note"), 0, 0, 0);
        QVERIFY(document.canSaveAnnotationsToSidecar());
        QVERIFY2(document.saveAnnotationsToSidecar(&error), qPrintable(error));
        QCOMPARE(Okular::AnnotationSidecar::pdfHash(embeddedPath, &error), hash);
        document.closeDocument();

        QCOMPARE(document.openDocument(embeddedPath, QUrl::fromLocalFile(embeddedPath), mime), Okular::Document::OpenSuccess);
        auto *overridden = document.page(0)->annotation(id);
        QVERIFY(overridden);
        QVERIFY(document.hasSeparatePdfAnnotations());
        QCOMPARE(overridden->contents(), QStringLiteral("updated note"));
        document.removePageAnnotation(0, overridden);
        QVERIFY(document.canSaveAnnotationsToSidecar());
        QVERIFY2(document.saveAnnotationsToSidecar(&error), qPrintable(error));
        document.closeDocument();

        QCOMPARE(document.openDocument(embeddedPath, QUrl::fromLocalFile(embeddedPath), mime), Okular::Document::OpenSuccess);
        QVERIFY(!document.page(0)->annotation(id));
        QCOMPARE(Okular::AnnotationSidecar::pdfHash(embeddedPath, &error), hash);
        document.closeDocument();
        QFile::remove(Okular::AnnotationSidecar::pathForHash(hash));
    }
};

QTEST_MAIN(AnnotationSidecarTest)

#include "annotationsidecartest.moc"
