/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include <QFile>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cstdlib>
#include <mdict_extern.h>
#include <zlib.h>

namespace
{
QByteArray number(quint64 value, int width = 8)
{
    QByteArray result(width, '\0');
    for (int i = width - 1; i >= 0; --i) {
        result[i] = value & 255;
        value >>= 8;
    }
    return result;
}
quint32 checksum(const QByteArray &bytes)
{
    return adler32(1, reinterpret_cast<const Bytef *>(bytes.constData()), bytes.size());
}
QByteArray block(const QByteArray &bytes, bool raw = false)
{
    QByteArray compressed;
    if (raw) {
        compressed = bytes;
    } else {
        uLongf length = compressBound(bytes.size());
        compressed.resize(length);
        const int error = compress2(reinterpret_cast<Bytef *>(compressed.data()), &length, reinterpret_cast<const Bytef *>(bytes.constData()), bytes.size(), Z_BEST_COMPRESSION);
        Q_ASSERT(error == Z_OK);
        compressed.resize(length);
    }
    return QByteArray::fromHex(raw ? "00000000" : "02000000") + number(checksum(bytes), 4) + compressed;
}
QByteArray encoded(const QString &text, bool utf16)
{
    QStringEncoder encoder(utf16 ? QStringEncoder::Utf16LE : QStringEncoder::Utf8);
    return encoder(text);
}
struct Fixture {
    QByteArray data;
    qsizetype keyHeader, index, keyBlock, recordHeader, recordBlock;
};
Fixture dictionary(bool utf16 = false, bool raw = false, bool split = false, bool aliases = false, bool missingTerminator = false, bool splitKeys = false, bool version2 = true)
{
    const int width = version2 ? 8 : 4;
    const auto integer = [width](quint64 value) { return number(value, width); };
    const QString terminator = version2 ? QString(QChar::Null) : QString();
    const QByteArray first = encoded(QStringLiteral("first definition") + QChar::Null, utf16);
    const QByteArray last = encoded(QStringLiteral("最后的定义") + QChar::Null, utf16);
    const QByteArray record = first + last;
    QString header = QStringLiteral("<Dictionary GeneratedByEngineVersion=\"%1\" Encoding=\"%2\" Encrypted=\"0\"/>").arg(version2 ? QStringLiteral("2.0") : QStringLiteral("1.2"), utf16 ? QStringLiteral("UTF-16") : QStringLiteral("UTF-8"));
    const auto text = encoded(header, true);
    // MDX's XML header checksum is little-endian; block checksums are big-endian.
    auto hash = number(checksum(text), 4);
    std::reverse(hash.begin(), hash.end());
    Fixture fixture;
    fixture.data = number(text.size(), 4) + text + hash;
    QByteArray firstKeys;
    if (aliases) {
        firstKeys += integer(0) + encoded(QStringLiteral("alias") + QChar::Null, utf16);
    }
    firstKeys += integer(0) + encoded(QStringLiteral("alpha") + QChar::Null, utf16);
    const auto lastKey = integer(first.size()) + encoded(QStringLiteral("beta") + (missingTerminator ? QString() : QString(QChar::Null)), utf16);
    const QString firstKey = aliases ? QStringLiteral("alias") : QStringLiteral("alpha");
    const quint64 count = aliases ? 3 : 2;
    QByteArray info, keyBlocks;
    const auto appendKeys = [&](const QByteArray &data, const QString &begin, const QString &end, quint64 entries) {
        const auto compressed = block(data, raw);
        info += integer(entries) + number(begin.size(), version2 ? 2 : 1) + encoded(begin + terminator, utf16) + number(end.size(), version2 ? 2 : 1) + encoded(end + terminator, utf16) + integer(compressed.size()) + integer(data.size());
        keyBlocks += compressed;
    };
    if (splitKeys) {
        appendKeys(firstKeys, firstKey, QStringLiteral("alpha"), count - 1);
        appendKeys(lastKey, QStringLiteral("beta"), QStringLiteral("beta"), 1);
    } else {
        appendKeys(firstKeys + lastKey, firstKey, QStringLiteral("beta"), count);
    }
    const auto index = version2 ? block(info) : info;
    const auto keyHeader = integer(splitKeys ? 2 : 1) + integer(count) + (version2 ? integer(info.size()) : QByteArray()) + integer(index.size()) + integer(keyBlocks.size());
    fixture.keyHeader = fixture.data.size();
    fixture.data += keyHeader + (version2 ? number(checksum(keyHeader), 4) : QByteArray());
    fixture.index = fixture.data.size();
    fixture.data += index;
    fixture.keyBlock = fixture.data.size();
    fixture.data += keyBlocks;
    fixture.recordHeader = fixture.data.size();
    const auto record1 = block(split ? record.first(8) : record, raw);
    const auto record2 = split ? block(record.sliced(8), raw) : QByteArray();
    fixture.data += integer(split ? 2 : 1) + integer(count) + integer((split ? 2 : 1) * width * 2) + integer(record1.size() + record2.size());
    fixture.data += integer(record1.size()) + integer(split ? 8 : record.size());
    if (split) {
        fixture.data += integer(record2.size()) + integer(record.size() - 8);
    }
    fixture.recordBlock = fixture.data.size();
    fixture.data += record1 + record2;
    return fixture;
}
QByteArray lookup(void *dict, const char *word)
{
    const auto result = mdict_lookup(dict, word);
    const QByteArray bytes(static_cast<const char *>(result.data), result.size);
    std::free(const_cast<void *>(result.data));
    return bytes;
}
}

class MdictParserTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void validBlocks_data()
    {
        QTest::addColumn<bool>("utf16");
        QTest::addColumn<bool>("raw");
        QTest::addColumn<bool>("split");
        QTest::addColumn<bool>("aliases");
        QTest::addColumn<bool>("splitKeys");
        QTest::addColumn<bool>("version2");
        QTest::newRow("zlib-last-record") << false << false << false << false << false << true;
        QTest::newRow("raw-last-record") << false << true << false << false << false << true;
        QTest::newRow("spanning-record") << false << false << true << false << false << true;
        QTest::newRow("utf16") << true << false << false << false << false << true;
        QTest::newRow("utf16-spanning-record") << true << false << true << false << false << true;
        QTest::newRow("synonyms") << false << false << false << true << false << true;
        QTest::newRow("key-block-boundary") << false << false << false << false << true << true;
        QTest::newRow("utf16-key-boundary") << true << false << true << false << true << true;
        QTest::newRow("legacy-utf8") << false << false << true << false << true << false;
        QTest::newRow("legacy-utf16") << true << true << true << false << true << false;
    }
    void validBlocks()
    {
        QFETCH(bool, utf16);
        QFETCH(bool, raw);
        QFETCH(bool, split);
        QFETCH(bool, aliases);
        QFETCH(bool, splitKeys);
        QFETCH(bool, version2);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("tiny.mdx"));
        const auto fixture = dictionary(utf16, raw, split, aliases, false, splitKeys, version2);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(fixture.data), fixture.data.size());
        file.close();
        void *dict = const_cast<void *>(mdict_init(QFile::encodeName(path).constData()).data);
        QVERIFY(dict);
        for (int i = 0; i < 50; ++i) {
            QCOMPARE(lookup(dict, "ALPHA"), QByteArray("first definition\0", 17));
            QCOMPARE(lookup(dict, "beta"), QStringLiteral("最后的定义").toUtf8() + '\0');
            QVERIFY(lookup(dict, "missing").isEmpty());
        }
        if (aliases) {
            QCOMPARE(lookup(dict, "alias"), lookup(dict, "alpha"));
        }
        uint64_t count = 0;
        auto **keys = mdict_keylist(dict, &count);
        QVERIFY(keys);
        QCOMPARE(count, quint64(aliases ? 3 : 2));
        free_simple_key_list(keys, count);
        QCOMPARE(mdict_destroy(dict), 0);
    }
    void malformedFiles()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("invalid.mdx"));
        const auto valid = dictionary();
        auto probe = [&](const QByteArray &bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
                return false;
            }
            file.close();
            void *dict = const_cast<void *>(mdict_init(QFile::encodeName(path).constData()).data);
            if (dict) {
                lookup(dict, "alpha");
                lookup(dict, "beta");
                mdict_destroy(dict);
            }
            return true;
        };
        for (qsizetype size = 0; size < valid.data.size(); ++size) {
            QVERIFY(probe(valid.data.first(size)));
            QVERIFY(!mdict_init(QFile::encodeName(path).constData()).data);
        }
        // Exercise every binary field, including valid header checksums that
        // lead to corrupt key/record metadata and malformed zlib streams.
        for (qsizetype i = 0; i < valid.data.size(); ++i) {
            auto bytes = valid.data;
            bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ 0xff);
            QVERIFY(probe(bytes));
        }
        QVERIFY(probe(dictionary(false, false, false, false, true).data));
        void *dict = const_cast<void *>(mdict_init(QFile::encodeName(path).constData()).data);
        QVERIFY(dict);
        QVERIFY(lookup(dict, "beta").isEmpty());
        mdict_destroy(dict);
        auto inconsistent = valid.data;
        inconsistent.replace(valid.recordHeader + 40, 8, number(100));
        QVERIFY(probe(inconsistent));
        dict = const_cast<void *>(mdict_init(QFile::encodeName(path).constData()).data);
        QVERIFY(dict);
        QVERIFY(lookup(dict, "alpha").isEmpty());
        QVERIFY(lookup(dict, "beta").isEmpty());
        mdict_destroy(dict);
        auto oversized = valid.data;
        oversized.replace(valid.recordHeader + 40, 8, number(quint64(1) << 40));
        QVERIFY(probe(oversized));
        QVERIFY(!mdict_init(QFile::encodeName(path).constData()).data);
    }
    void nullArguments()
    {
        QVERIFY(!mdict_init(nullptr).data);
        QVERIFY(!mdict_lookup(nullptr, "alpha").data);
        QVERIFY(!mdict_locate(nullptr, nullptr, MDICT_ENCODING_HEX).data);
        QVERIFY(!mdict_parse_definition(nullptr, nullptr, 0).data);
        QVERIFY(!mdict_keylist(nullptr, nullptr));
        QCOMPARE(free_simple_key_list(nullptr, 0), 0);
    }
};
QTEST_GUILESS_MAIN(MdictParserTest)
#include "mdictparsertest.moc"
