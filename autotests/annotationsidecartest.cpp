/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QFile>
#include <QMimeDatabase>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "../core/annotationsidecar_p.h"
#include "../core/annotations.h"
#include "../core/document.h"
#include "../core/page.h"
#include "../settings_core.h"

class AnnotationSidecarTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void replayAndQuery()
    {
        const QString hash = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
            QVERIFY(query.exec(QStringLiteral("SELECT operation FROM events ORDER BY sequence")));
            QStringList operations;
            while (query.next()) {
                operations.append(query.value(0).toString());
            }
            QCOMPARE(operations, QStringList({QStringLiteral("upsert"), QStringLiteral("upsert"), QStringLiteral("delete"), QStringLiteral("upsert")}));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);

        const QString differentHash = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY2(Okular::AnnotationSidecar::load(differentHash, &loaded, &error), qPrintable(error));
        QVERIFY(loaded.isEmpty());
        QVERIFY(QFile::copy(Okular::AnnotationSidecar::pathForHash(hash), Okular::AnnotationSidecar::pathForHash(differentHash)));
        QVERIFY(!Okular::AnnotationSidecar::load(differentHash, &loaded, &error));
        QVERIFY(loaded.isEmpty());
        QFile::remove(Okular::AnnotationSidecar::pathForHash(hash));
        QFile::remove(Okular::AnnotationSidecar::pathForHash(differentHash));
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
