/*
 * Copyright (c) 2025-Present
 * This code is licensed under the BSD 3-Clause License.
 * See the LICENSE file for details.
 */
#include "include/mdict_extern.h"
#include "include/mdict.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>

std::string mime_detect(const std::string &filename)
{
    static const std::unordered_map<std::string, std::string> mime_map = {
        {"png", "image/png"},  {"jpg", "image/jpeg"}, {"gif", "image/gif"},  {"ico", "image/x-icon"}, {"jpeg", "image/jpeg"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"},
        {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"},  {"wav", "audio/wav"},  {"m4a", "audio/m4a"},    {"m4v", "video/m4v"},   {"m4b", "audio/m4b"},   {"js", "application/javascript"},
        {"css", "text/css"},   {"html", "text/html"}, {"txt", "text/plain"}, {"ttf", "font/ttf"},     {"otf", "font/otf"},    {"woff", "font/woff"},  {"woff2", "font/woff2"}};

    // Find the last dot in the filename
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream"; // Default MIME type for unknown
                                           // extensions
    }

    // Get the extension and convert to lowercase
    std::string ext = filename.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Look up the MIME type
    auto it = mime_map.find(ext);
    if (it != mime_map.end()) {
        return it->second;
    }

    return "application/octet-stream"; // Default MIME type for unknown
                                       // extensions
}

namespace
{
SizedData copyResult(const std::string &text)
{
    auto *data = static_cast<char *>(std::malloc(text.size() + 1));
    if (!data) {
        return {nullptr, 0};
    }
    std::memcpy(data, text.c_str(), text.size() + 1);
    return {data, text.size()};
}

template<typename Function> SizedData resultOrEmpty(Function function) noexcept
{
    try {
        return copyResult(function());
    } catch (...) {
        return {nullptr, 0};
    }
}
}

extern "C" {
SizedData mdict_init(const char *path)
{
    if (!path) {
        return {nullptr, 0};
    }
    try {
        auto dict = std::make_unique<mdict::Mdict>(path);
        dict->init();
        return {dict.release(), sizeof(mdict::Mdict *)};
    } catch (...) {
        return {nullptr, 0};
    }
}

SizedData mdict_lookup(void *dict, const char *word)
{
    if (!dict || !word) {
        return {nullptr, 0};
    }
    return resultOrEmpty([&] { return static_cast<mdict::Mdict *>(dict)->lookup(word); });
}

SizedData mdict_locate(void *dict, const char *word, mdict_encoding_t encoding)
{
    if (!dict || !word) {
        return {nullptr, 0};
    }
    return resultOrEmpty([&] { return static_cast<mdict::Mdict *>(dict)->locate(word, encoding); });
}

SizedData mdict_parse_definition(void *dict, const char *word, unsigned long start)
{
    if (!dict || !word) {
        return {nullptr, 0};
    }
    return resultOrEmpty([&] { return static_cast<mdict::Mdict *>(dict)->parse_definition(word, start); });
}

int free_simple_key_list(simple_key_item **items, uint64_t len)
{
    if (items) {
        for (uint64_t i = 0; i < len; ++i) {
            if (items[i]) {
                std::free(items[i]->key_word);
                delete items[i];
            }
        }
        delete[] items;
    }
    return 0;
}

simple_key_item **mdict_keylist(void *dict, uint64_t *len)
{
    if (!len) {
        return nullptr;
    }
    *len = 0;
    if (!dict) {
        return nullptr;
    }
    simple_key_item **items = nullptr;
    uint64_t count = 0;
    try {
        const auto list = static_cast<mdict::Mdict *>(dict)->keyList();
        count = list.size();
        items = new simple_key_item *[count]();
        for (size_t i = 0; i < count; ++i) {
            items[i] = new simple_key_item {list[i].record_start, nullptr};
            items[i]->key_word = static_cast<char *>(const_cast<void *>(copyResult(list[i].key_word).data));
            if (!items[i]->key_word) {
                throw std::bad_alloc();
            }
        }
        *len = count;
        return items;
    } catch (...) {
        free_simple_key_list(items, count);
        return nullptr;
    }
}

int mdict_filetype(void *dict)
{
    return dict && static_cast<mdict::Mdict *>(dict)->filetype == "MDX" ? 0 : 1;
}

void mdict_suggest(void *, char *, char **, int)
{
}
void mdict_stem(void *, char *, char **, int)
{
}
int mdict_destroy(void *dict)
{
    delete static_cast<mdict::Mdict *>(dict);
    return dict ? 0 : -1;
}

SizedData c_mime_detect(const char *filename)
{
    if (!filename) {
        return {nullptr, 0};
    }
    try {
        thread_local std::string result;
        result = mime_detect(filename);
        return {result.c_str(), result.size()};
    } catch (...) {
        return {nullptr, 0};
    }
}

char *mdict_atomic_lookup(const char *path, const char *key)
{
    if (!path || !key) {
        return nullptr;
    }
    return static_cast<char *>(const_cast<void *>(resultOrEmpty([&] {
                                                      mdict::Mdict dict(path);
                                                      dict.init();
                                                      return dict.lookup(key);
                                                  }).data));
}
}
