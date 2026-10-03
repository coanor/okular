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
    void testWordNormalization();
    void testFileLookup();
    void testSwitchDictionary();
};

void MdxDictionaryTest::testWordNormalization()
{
    QCOMPARE(MdxDictionary::word(QStringLiteral("inter-\nnational")), QStringLiteral("international"));
    QCOMPARE(MdxDictionary::word(QStringLiteral("inter- \r\n  national")), QStringLiteral("international"));
    QCOMPARE(MdxDictionary::word(QStringLiteral("prin-\nted")), QStringLiteral("printed"));
    QCOMPARE(MdxDictionary::word(QStringLiteral("prin\u00ad\nted")), QStringLiteral("printed"));
    QCOMPARE(MdxDictionary::word(QStringLiteral("well-known")), QStringLiteral("well-known"));
    QVERIFY(MdxDictionary::word(QStringLiteral("two words")).isEmpty());
}

void MdxDictionaryTest::testFileLookup()
{
    const QString path = qEnvironmentVariable("OKULAR_TEST_MDX");
    if (path.isEmpty()) {
        QSKIP("Set OKULAR_TEST_MDX to an ECDICT MDX file to test dictionary lookup");
    }
    const QFileInfo info(path);
    QVERIFY(info.isFile());
    QCOMPARE(info.suffix(), QStringLiteral("mdx"));

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
    const QString source = qEnvironmentVariable("OKULAR_TEST_MDX");
    if (source.isEmpty()) {
        QSKIP("Set OKULAR_TEST_MDX to an ECDICT MDX file to test dictionary switching");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString local = directory.filePath(QStringLiteral("custom.mdx"));
    QVERIFY(QFile::copy(source, local));

    const QString definition = MdxDictionary::lookup(source, QStringLiteral("dictionary"));
    QVERIFY(!definition.isEmpty());
    QCOMPARE(MdxDictionary::lookup(local, QStringLiteral("dictionary")), definition);
    QCOMPARE(MdxDictionary::lookup(source, QStringLiteral("dictionary")), definition);
    QVERIFY(MdxDictionary::lookup(source, QStringLiteral("okular_nonexistent_word_12345")).isEmpty());
    MdxDictionary::invalidate(source);
}

QTEST_MAIN(MdxDictionaryTest)
#include "mdxdictionarytest.moc"
