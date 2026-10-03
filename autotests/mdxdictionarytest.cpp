/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "part/mdxdictionary.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class MdxDictionaryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testBundledLookup();
    void testSwitchDictionary();
};

void MdxDictionaryTest::testBundledLookup()
{
    const QString path = QStringLiteral(":/okular/dictionaries/default.mdx");
    if (!QFile::exists(path)) {
        QSKIP("This build does not bundle ECDICT");
    }
    const QFileInfo info(path);
    QVERIFY(info.isFile());
    QCOMPARE(info.suffix(), QStringLiteral("mdx"));
    QVERIFY(QFile::exists(QStringLiteral(":/okular/dictionaries/LICENSE")));

    const QString definition = MdxDictionary::lookup(path, QStringLiteral("hello"));
    QVERIFY(!definition.isEmpty());
    const QString summary = MdxDictionary::summary(definition);
    QVERIFY2(summary.contains(QStringLiteral("哈罗")), qPrintable(summary));
    QCOMPARE(MdxDictionary::lookup(path, QStringLiteral("hello")), definition);
    MdxDictionary::invalidate(path);
    QCOMPARE(MdxDictionary::lookup(path, QStringLiteral("hello")), definition);
}

void MdxDictionaryTest::testSwitchDictionary()
{
    const QString bundled = QStringLiteral(":/okular/dictionaries/default.mdx");
    if (!QFile::exists(bundled)) {
        QSKIP("This build does not bundle ECDICT");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString local = directory.filePath(QStringLiteral("custom.mdx"));
    QVERIFY(QFile::copy(bundled, local));

    const QString definition = MdxDictionary::lookup(bundled, QStringLiteral("dictionary"));
    QVERIFY(!definition.isEmpty());
    QCOMPARE(MdxDictionary::lookup(local, QStringLiteral("dictionary")), definition);
    QCOMPARE(MdxDictionary::lookup(bundled, QStringLiteral("dictionary")), definition);
    QVERIFY(MdxDictionary::lookup(bundled, QStringLiteral("okular_nonexistent_word_12345")).isEmpty());
    MdxDictionary::invalidate(bundled);
}

QTEST_MAIN(MdxDictionaryTest)
#include "mdxdictionarytest.moc"
