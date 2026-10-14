/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright 2026 Fcitx5 for Android Contributors
 */
#ifndef FCITX5_ANDROID_BOSHIAMY_DICTIONARY_H
#define FCITX5_ANDROID_BOSHIAMY_DICTIONARY_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fcitx {

/**
 * One Boshiamy radical table, e.g. converted from liu-uni.tab.
 *
 * File format (UTF-8): one "<code> <word>" per line, candidates for the same
 * code appear in priority order. Lines starting with '#' are ignored.
 */
class BoshiamyTable {
public:
    bool load(const std::string &path);
    // Merge additional entries (e.g. user custom table), appended after
    // existing candidates of the same code.
    bool merge(const std::string &path);

    bool empty() const { return words_.empty(); }
    size_t size() const { return entries_; }

    const std::vector<std::string> *lookup(const std::string &code) const;

    /**
     * Wildcard search. '?' matches exactly one radical, '*' matches zero or
     * more radicals. Returns (word, code) pairs ordered by code length and
     * code, at most limit results.
     */
    std::vector<std::pair<std::string, std::string>>
    wildcard(const std::string &pattern, size_t limit) const;

private:
    bool parse(const std::string &path);

    std::unordered_map<std::string, std::vector<std::string>> words_;
    std::vector<std::string> sortedCodes_;
    size_t entries_ = 0;
};

/**
 * Zhuyin (Bopomofo) reading table, from UCL_LIU's pinyi.txt (VERSION_0.01).
 *
 * Line 1: "VERSION_0.01"
 * Line 2: space separated keyboard keys
 * Line 3: space separated Bopomofo symbols, matching line 2
 * Rest:   "<zhuyin keys> <char> <char> ..." (tone 1 has no tone key)
 */
class BoshiamyPhonetic {
public:
    bool load(const std::string &path);
    bool loaded() const { return !keyToSymbol_.empty(); }

    // Keyboard key -> Bopomofo symbol, e.g. '1' -> "ㄅ".
    std::optional<std::string> symbolForKey(char key) const;
    // Bopomofo symbol -> keyboard key.
    std::optional<char> keyForSymbol(const std::string &symbol) const;

    // Characters for a zhuyin key sequence, e.g. "z6" (ㄈㄟˊ).
    const std::vector<std::string> *lookup(const std::string &keys) const;

    // All homophones of word, ordered like UCL_LIU (closest first).
    std::vector<std::string> homophones(const std::string &word) const;

private:
    std::unordered_map<char, std::string> keyToSymbol_;
    std::unordered_map<std::string, char> symbolToKey_;
    std::vector<std::vector<std::string>> lines_;
    std::unordered_map<std::string, size_t> keysToLine_;
    // word -> list of (line index, position in that line)
    std::unordered_map<std::string, std::vector<std::pair<size_t, size_t>>>
        wordIndex_;
};

} // namespace fcitx

#endif // FCITX5_ANDROID_BOSHIAMY_DICTIONARY_H
