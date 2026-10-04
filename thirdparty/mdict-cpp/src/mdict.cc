/*
 * Copyright (c) 2025-Present
 * This code is licensed under the BSD 3-Clause License.
 * See the LICENSE file for details.
 */
#include "include/mdict.h"

#include <QByteArray>
#include <QFile>
#include <QStringConverter>
#include <QXmlStreamReader>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <zlib.h>

#include "include/ripemd128.h"

namespace
{
constexpr uint64_t MaxHeader = 1024 * 1024;
constexpr uint64_t MaxIndex = 16 * 1024 * 1024;
constexpr uint64_t MaxBlock = 64 * 1024 * 1024;
constexpr uint64_t MaxDefinition = 1024 * 1024;
constexpr uint64_t MaxBlocks = 100000;

void require(bool condition)
{
    if (!condition) {
        throw std::runtime_error("Invalid or unsupported MDX dictionary");
    }
}

// A cursor never constructs a pointer beyond its byte array.
class Cursor
{
public:
    explicit Cursor(QByteArray data)
        : data(std::move(data))
    {
    }
    uint64_t number(int width)
    {
        require(width > 0 && width <= 8 && width <= data.size() - offset);
        uint64_t result = 0;
        for (int i = 0; i < width; ++i) {
            result = (result << 8) | static_cast<unsigned char>(data[offset++]);
        }
        return result;
    }
    QByteArray take(uint64_t length)
    {
        require(length <= static_cast<uint64_t>(data.size() - offset));
        const auto result = data.mid(offset, length);
        offset += length;
        return result;
    }
    QByteArray terminated(int width)
    {
        const qsizetype start = offset;
        while (offset <= data.size() - width) {
            const bool terminator = data[offset] == '\0' && (width == 1 || data[offset + 1] == '\0');
            offset += width;
            require(offset - start <= 65536);
            if (terminator) {
                return data.mid(start, offset - start - width);
            }
        }
        throw std::runtime_error("Unterminated MDX key");
    }
    bool empty() const
    {
        return offset == data.size();
    }

private:
    QByteArray data;
    qsizetype offset = 0;
};

uint32_t checksum(const QByteArray &data)
{
    return adler32(1, reinterpret_cast<const Bytef *>(data.constData()), data.size());
}

QByteArray decodeBlock(QByteArray compressed, uint64_t expected, bool encrypted = false)
{
    require(compressed.size() >= 8 && expected > 0 && expected <= MaxBlock);
    Cursor header(compressed.first(8));
    const uint64_t type = header.number(4);
    const uint32_t hash = header.number(4);
    if (encrypted) {
        // RIPEMD-128 of the eight-byte seed, including standard MD padding.
        // Stack-local state avoids the upstream hash helper's shared buffer.
        dword32 words[16] = {};
        const auto *bytes = reinterpret_cast<const unsigned char *>(compressed.constData());
        words[0] = BYTES_TO_DWORD(bytes + 4);
        words[1] = 0x3695;
        words[2] = 0x80;
        words[14] = 64;
        dword32 digest[4];
        ripemd128Init(digest);
        ripemd128compress(digest, words);
        unsigned char previous = 0x36;
        for (qsizetype i = 8; i < compressed.size(); ++i) {
            const unsigned char original = compressed[i];
            const auto key = static_cast<unsigned char>(digest[((i - 8) % 16) / 4] >> (((i - 8) % 4) * 8));
            compressed[i] = static_cast<char>(((original >> 4) | (original << 4)) ^ previous ^ ((i - 8) & 255) ^ key);
            previous = original;
        }
    }
    QByteArray output;
    if (type == 0) {
        output = compressed.sliced(8);
        require(static_cast<uint64_t>(output.size()) == expected);
    } else {
        // Block types are stored little-endian: 02 00 00 00 is zlib.
        require(type == 0x02000000);
        output.resize(expected);
        uLongf size = output.size();
        require(uncompress(reinterpret_cast<Bytef *>(output.data()), &size, reinterpret_cast<const Bytef *>(compressed.constData() + 8), compressed.size() - 8) == Z_OK);
        require(size == expected);
    }
    require(checksum(output) == hash);
    return output;
}

QString decodeText(const QByteArray &data, bool utf16)
{
    QStringDecoder decoder(utf16 ? QStringDecoder::Utf16LE : QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(data);
    require(!decoder.hasError() && (!utf16 || data.size() % 2 == 0));
    return text;
}
}

namespace mdict
{
struct Mdict::Impl {
    struct KeyBlock {
        uint64_t offset, compressed, decompressed, count;
        std::string first, last;
    };
    struct RecordBlock {
        uint64_t offset, compressed, decompressed, start;
    };
    explicit Impl(const std::string &filename)
        : file(QFile::decodeName(filename.c_str()))
    {
    }
    QFile file;
    uint64_t fileSize = 0;
    uint64_t position = 0;
    uint64_t totalRecords = 0;
    uint64_t entryCount = 0;
    bool utf16 = false;
    bool version2 = true;
    bool caseSensitive = false;
    bool stripKey = true;
    std::vector<KeyBlock> keys;
    std::vector<RecordBlock> records;
    size_t cachedKey = std::numeric_limits<size_t>::max();
    std::vector<key_list_item> cachedItems;

    QByteArray read(uint64_t offset, uint64_t size, uint64_t limit = MaxBlock)
    {
        require(size <= limit && offset <= fileSize && size <= fileSize - offset);
        require(file.seek(offset));
        QByteArray result = file.read(size);
        require(static_cast<uint64_t>(result.size()) == size);
        return result;
    }
    QByteArray next(uint64_t size, uint64_t limit = MaxBlock)
    {
        auto result = read(position, size, limit);
        position += size;
        return result;
    }
    void skip(uint64_t size)
    {
        require(position <= fileSize && size <= fileSize - position);
        position += size;
    }
    std::string normalized(const std::string &word) const
    {
        QString text = QString::fromUtf8(word.data(), word.size());
        if (!caseSensitive) {
            text = text.toLower();
        }
        if (stripKey) {
            QString filtered;
            for (const QChar c : text) {
                if (!c.isSpace() && !QStringLiteral(":.,-_'()#<>!").contains(c)) {
                    filtered += c;
                }
            }
            text = std::move(filtered);
        }
        return text.toStdString();
    }
    std::string indexedText(Cursor &cursor) const
    {
        const uint64_t length = cursor.number(version2 ? 2 : 1);
        auto data = cursor.take(length * (utf16 ? 2 : 1));
        if (version2) {
            require(cursor.take(utf16 ? 2 : 1) == QByteArray(utf16 ? 2 : 1, '\0'));
        }
        return decodeText(data, utf16).toStdString();
    }
    const std::vector<key_list_item> &items(size_t block, uint64_t &budget)
    {
        require(block < keys.size());
        if (cachedKey == block) {
            return cachedItems;
        }
        const auto &meta = keys[block];
        require(meta.decompressed <= budget);
        budget -= meta.decompressed;
        Cursor cursor(decodeBlock(read(meta.offset, meta.compressed), meta.decompressed));
        std::vector<key_list_item> result;
        require(meta.count <= meta.decompressed / (version2 ? 9 : 5));
        result.reserve(meta.count);
        for (uint64_t i = 0; i < meta.count; ++i) {
            const uint64_t offset = cursor.number(version2 ? 8 : 4);
            const auto text = decodeText(cursor.terminated(utf16 ? 2 : 1), utf16).toStdString();
            require(offset < totalRecords && (result.empty() || offset >= result.back().record_start));
            result.push_back({offset, text});
        }
        require(cursor.empty() && !result.empty());
        require(normalized(result.front().key_word) == meta.first && normalized(result.back().key_word) == meta.last);
        cachedItems = std::move(result);
        cachedKey = block;
        return cachedItems;
    }
    std::pair<uint64_t, uint64_t> find(const std::string &word)
    {
        require(word.size() <= 65536);
        const auto query = normalized(word);
        uint64_t budget = MaxBlock;
        for (size_t block = 0; block < keys.size(); ++block) {
            if (query < keys[block].first || query > keys[block].last) {
                continue;
            }
            const auto &list = items(block, budget);
            auto found = std::find_if(list.begin(), list.end(), [&](const auto &item) { return normalized(item.key_word) == query; });
            if (found == list.end()) {
                continue;
            }
            const uint64_t start = found->record_start;
            // Synonyms can share the same offset. Definitions may span blocks.
            size_t index = std::distance(list.begin(), found) + 1;
            for (size_t nextBlock = block; nextBlock < keys.size(); ++nextBlock) {
                const auto &nextItems = items(nextBlock, budget);
                for (; index < nextItems.size(); ++index) {
                    const uint64_t end = nextItems[index].record_start;
                    require(end >= start);
                    if (end > start) {
                        return {start, end};
                    }
                }
                index = 0;
            }
            return {start, totalRecords};
        }
        return {0, 0};
    }
    QByteArray definition(uint64_t start, uint64_t end)
    {
        require(start <= end && end <= totalRecords && end - start <= MaxDefinition);
        QByteArray result;
        uint64_t budget = MaxBlock;
        for (const auto &block : records) {
            if (start >= block.start + block.decompressed || end <= block.start) {
                continue;
            }
            require(block.decompressed <= budget);
            budget -= block.decompressed;
            const auto data = decodeBlock(read(block.offset, block.compressed), block.decompressed);
            const uint64_t begin = std::max(start, block.start) - block.start;
            const uint64_t count = std::min(end, block.start + block.decompressed) - block.start - begin;
            result += data.mid(begin, count);
        }
        require(static_cast<uint64_t>(result.size()) == end - start);
        return result;
    }
};

Mdict::Mdict(std::string filename)
    : filetype(QString::fromStdString(filename).endsWith(QLatin1String(".mdd"), Qt::CaseInsensitive) ? "MDD" : "MDX")
    , d(std::make_unique<Impl>(filename))
{
}
Mdict::~Mdict() = default;

void Mdict::init()
{
    require(d->file.open(QIODevice::ReadOnly));
    d->fileSize = d->file.size();
    Cursor size(d->next(4));
    const auto header = d->next(size.number(4), MaxHeader);
    const auto headerChecksum = d->next(4);
    uint32_t stored = 0;
    for (int i = 3; i >= 0; --i) {
        stored = (stored << 8) | static_cast<unsigned char>(headerChecksum[i]);
    }
    require(checksum(header) == stored);
    QString headerText = decodeText(header, true);
    while (headerText.endsWith(QChar::Null)) {
        headerText.chop(1);
    }
    QXmlStreamReader xml(headerText);
    require(xml.readNextStartElement());
    const auto attributes = xml.attributes();
    const QString version = attributes.value(QLatin1String("GeneratedByEngineVersion")).toString();
    bool validVersion = false;
    const double engine = version.toDouble(&validVersion);
    require(validVersion && engine >= 1 && engine < 3);
    d->version2 = engine >= 2;
    const int width = d->version2 ? 8 : 4;
    const QString encryption = attributes.value(QLatin1String("Encrypted")).toString();
    require(encryption.isEmpty() || encryption == QLatin1String("No") || encryption == QLatin1String("0") || encryption == QLatin1String("2"));
    const QString encoding = attributes.value(QLatin1String("Encoding")).toString().toUpper();
    d->utf16 = filetype == "MDD" || encoding == QLatin1String("UTF-16") || encoding == QLatin1String("UTF16");
    require(d->utf16 || encoding.isEmpty() || encoding == QLatin1String("UTF-8"));
    d->caseSensitive = attributes.value(QLatin1String("KeyCaseSensitive")) == QLatin1String("Yes");
    d->stripKey = attributes.value(QLatin1String("StripKey")) != QLatin1String("No");
    xml.skipCurrentElement();
    while (!xml.atEnd()) {
        xml.readNext();
    }
    require(!xml.hasError());

    const auto keyHeader = d->next(width * (d->version2 ? 5 : 4));
    Cursor keyNumbers(keyHeader);
    const uint64_t keyCount = keyNumbers.number(width);
    d->entryCount = keyNumbers.number(width);
    const uint64_t indexDecompressed = d->version2 ? keyNumbers.number(width) : 0;
    const uint64_t indexSize = keyNumbers.number(width);
    const uint64_t keySize = keyNumbers.number(width);
    require(keyCount > 0 && keyCount <= MaxBlocks && d->entryCount > 0 && indexDecompressed <= MaxIndex);
    if (d->version2) {
        Cursor hash(d->next(4));
        require(hash.number(4) == checksum(keyHeader));
    }
    auto index = d->next(indexSize, MaxIndex);
    if (d->version2) {
        index = decodeBlock(std::move(index), indexDecompressed, encryption == QLatin1String("2"));
    }
    Cursor keyIndex(std::move(index));
    uint64_t accumulated = 0;
    const uint64_t keyStart = d->position;
    for (uint64_t i = 0; i < keyCount; ++i) {
        const uint64_t count = keyIndex.number(width);
        const auto first = d->normalized(d->indexedText(keyIndex));
        const auto last = d->normalized(d->indexedText(keyIndex));
        const uint64_t compressed = keyIndex.number(width);
        const uint64_t decompressed = keyIndex.number(width);
        require(count > 0 && count <= 250000 && count <= d->entryCount - accumulated);
        require(compressed >= 8 && compressed <= MaxBlock && decompressed > 0 && decompressed <= MaxBlock);
        require(first <= last && (d->keys.empty() || d->keys.back().last <= last));
        d->keys.push_back({d->position, compressed, decompressed, count, first, last});
        d->skip(compressed);
        accumulated += count;
    }
    require(keyIndex.empty() && accumulated == d->entryCount && d->position - keyStart == keySize);

    Cursor recordNumbers(d->next(width * 4));
    const uint64_t recordCount = recordNumbers.number(width);
    require(recordCount > 0 && recordCount <= MaxBlocks && recordNumbers.number(width) == d->entryCount);
    const uint64_t recordIndexSize = recordNumbers.number(width);
    const uint64_t recordSize = recordNumbers.number(width);
    require(recordIndexSize == recordCount * width * 2);
    Cursor recordIndex(d->next(recordIndexSize, MaxIndex));
    const uint64_t recordStart = d->position;
    for (uint64_t i = 0; i < recordCount; ++i) {
        const uint64_t compressed = recordIndex.number(width);
        const uint64_t decompressed = recordIndex.number(width);
        require(compressed >= 8 && compressed <= MaxBlock && decompressed > 0 && decompressed <= MaxBlock);
        require(decompressed <= std::numeric_limits<uint64_t>::max() - d->totalRecords);
        d->records.push_back({d->position, compressed, decompressed, d->totalRecords});
        d->skip(compressed);
        d->totalRecords += decompressed;
    }
    require(d->position - recordStart == recordSize);
}

std::string Mdict::lookup(const std::string &word)
{
    const auto [start, end] = d->find(word);
    return decodeText(d->definition(start, end), d->utf16).toStdString();
}

std::string Mdict::locate(const std::string &word, mdict_encoding_t encoding)
{
    const auto [start, end] = d->find(word);
    const auto bytes = d->definition(start, end);
    return (encoding == MDICT_ENCODING_HEX ? bytes.toHex() : bytes.toBase64()).toStdString();
}

std::string Mdict::parse_definition(const std::string &word, uint64_t record_start)
{
    const auto [start, end] = d->find(word);
    require(start == record_start);
    return decodeText(d->definition(start, end), d->utf16).toStdString();
}

std::vector<key_list_item> Mdict::keyList()
{
    // This optional API enumerates keys; cap its total decoding work as well.
    require(d->entryCount <= 1000000);
    std::vector<key_list_item> result;
    uint64_t budget = MaxBlock;
    for (size_t i = 0; i < d->keys.size(); ++i) {
        const auto &items = d->items(i, budget);
        result.insert(result.end(), items.begin(), items.end());
    }
    return result;
}
}
