/*
 * Copyright (c) 2025-Present
 * This code is licensed under the BSD 3-Clause License.
 * See the LICENSE file for details.
 */
#pragma once

#include "mdict_extern.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mdict
{
struct key_list_item {
    uint64_t record_start;
    std::string key_word;
};

// The bundled parser owns its buffers and indexes. Only blocks needed for a
// lookup are decoded; file-controlled sizes are checked before allocation.
class Mdict
{
public:
    explicit Mdict(std::string filename);
    ~Mdict();
    void init();
    std::string lookup(const std::string &word);
    std::string locate(const std::string &word, mdict_encoding_t encoding);
    std::string parse_definition(const std::string &word, uint64_t record_start);
    std::vector<key_list_item> keyList();
    std::string filetype;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
