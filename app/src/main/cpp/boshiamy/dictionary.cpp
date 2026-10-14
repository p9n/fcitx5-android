/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright 2026 Fcitx5 for Android Contributors
 */
#include "dictionary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace fcitx {

namespace {

void trimLine(std::string &line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' ||
            line.back() == '\t')) {
        line.pop_back();
    }
    size_t start = 0;
    // Strip UTF-8 BOM
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        start = 3;
    }
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    if (start) {
        line.erase(0, start);
    }
}

std::vector<std::string> splitSpaces(const std::string &line) {
    std::vector<std::string> result;
    std::istringstream ss(line);
    std::string item;
    while (ss >> item) {
        result.push_back(std::move(item));
    }
    return result;
}

bool globMatch(std::string_view pattern, std::string_view code) {
    // Iterative glob matching with backtracking on '*'.
    size_t p = 0, c = 0, starP = std::string_view::npos, starC = 0;
    while (c < code.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == code[c])) {
            p++;
            c++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starC = c;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            c = ++starC;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }
    return p == pattern.size();
}

} // namespace

bool BoshiamyTable::parse(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        trimLine(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto pos = line.find_first_of(" \t");
        if (pos == std::string::npos) {
            continue;
        }
        std::string code = line.substr(0, pos);
        auto wordStart = line.find_first_not_of(" \t", pos);
        if (wordStart == std::string::npos) {
            continue;
        }
        std::transform(code.begin(), code.end(), code.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        auto [iter, inserted] = words_.try_emplace(code);
        if (inserted) {
            sortedCodes_.push_back(code);
        }
        iter->second.push_back(line.substr(wordStart));
        entries_++;
    }
    std::sort(sortedCodes_.begin(), sortedCodes_.end(),
              [](const std::string &a, const std::string &b) {
                  if (a.size() != b.size()) {
                      return a.size() < b.size();
                  }
                  return a < b;
              });
    return true;
}

bool BoshiamyTable::load(const std::string &path) {
    words_.clear();
    sortedCodes_.clear();
    entries_ = 0;
    return parse(path) && !words_.empty();
}

bool BoshiamyTable::merge(const std::string &path) { return parse(path); }

const std::vector<std::string> *
BoshiamyTable::lookup(const std::string &code) const {
    auto iter = words_.find(code);
    if (iter == words_.end()) {
        return nullptr;
    }
    return &iter->second;
}

std::vector<std::pair<std::string, std::string>>
BoshiamyTable::wildcard(const std::string &pattern, size_t limit) const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto &code : sortedCodes_) {
        if (!globMatch(pattern, code)) {
            continue;
        }
        for (const auto &word : words_.at(code)) {
            result.emplace_back(word, code);
            if (result.size() >= limit) {
                return result;
            }
        }
    }
    return result;
}

bool BoshiamyPhonetic::load(const std::string &path) {
    keyToSymbol_.clear();
    symbolToKey_.clear();
    lines_.clear();
    keysToLine_.clear();
    wordIndex_.clear();

    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    std::vector<std::string> keys;
    std::vector<std::string> symbols;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        trimLine(line);
        lineNo++;
        if (lineNo == 1) {
            if (line.rfind("VERSION_", 0) != 0) {
                return false;
            }
            continue;
        }
        if (lineNo == 2) {
            keys = splitSpaces(line);
            continue;
        }
        if (lineNo == 3) {
            symbols = splitSpaces(line);
            if (keys.size() != symbols.size()) {
                return false;
            }
            for (size_t i = 0; i < keys.size(); i++) {
                if (keys[i].size() != 1) {
                    continue;
                }
                keyToSymbol_[keys[i][0]] = symbols[i];
                symbolToKey_[symbols[i]] = keys[i][0];
            }
            continue;
        }
        auto items = splitSpaces(line);
        if (items.size() < 2) {
            continue;
        }
        const size_t idx = lines_.size();
        std::vector<std::string> chars(items.begin() + 1, items.end());
        for (size_t pos = 0; pos < chars.size(); pos++) {
            wordIndex_[chars[pos]].emplace_back(idx, pos);
        }
        keysToLine_.try_emplace(items[0], idx);
        lines_.push_back(std::move(chars));
    }
    return !keyToSymbol_.empty();
}

std::optional<std::string> BoshiamyPhonetic::symbolForKey(char key) const {
    auto iter = keyToSymbol_.find(key);
    if (iter == keyToSymbol_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::optional<char>
BoshiamyPhonetic::keyForSymbol(const std::string &symbol) const {
    auto iter = symbolToKey_.find(symbol);
    if (iter == symbolToKey_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

const std::vector<std::string> *
BoshiamyPhonetic::lookup(const std::string &keys) const {
    auto iter = keysToLine_.find(keys);
    if (iter == keysToLine_.end()) {
        return nullptr;
    }
    return &lines_[iter->second];
}

std::vector<std::string>
BoshiamyPhonetic::homophones(const std::string &word) const {
    std::vector<std::string> result;
    auto iter = wordIndex_.find(word);
    if (iter == wordIndex_.end()) {
        return result;
    }
    // Sort the readings by the position of the word in each line, so the
    // most common reading of the word comes first (same as UCL_LIU).
    auto occurrences = iter->second;
    std::stable_sort(
        occurrences.begin(), occurrences.end(),
        [](const auto &a, const auto &b) { return a.second < b.second; });
    std::unordered_set<std::string> seen;
    for (const auto &[lineIdx, pos] : occurrences) {
        for (const auto &ch : lines_[lineIdx]) {
            if (seen.insert(ch).second) {
                result.push_back(ch);
            }
        }
    }
    return result;
}

} // namespace fcitx
