/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright 2026 Fcitx5 for Android Contributors
 *
 * Boshiamy (嘸蝦米) input method engine.
 *
 * Behaviors follow UCL_LIU (https://github.com/shadowjohn/UCL_LIU):
 *  - radical keys: a-z , . ' [ ]
 *  - 0-9 selects candidates, Space commits the current candidate, or clears
 *    an invalid code
 *  - code + V/R/S/F/W selects the 2nd-6th candidate of code
 *  - "'<code>" + Space lists homophones of the first candidate
 *  - "';" enters Zhuyin lookup mode
 *  - ,,T / ,,C / ,,CT / ,,J switches between liu-uni{,2,3,4}.tab tables
 */
#include "engine.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>

namespace fcitx {

FCITX_DEFINE_LOG_CATEGORY(boshiamy_log, "boshiamy");
#define BOSHIAMY_DEBUG() FCITX_LOGC(boshiamy_log, Debug)
#define BOSHIAMY_INFO() FCITX_LOGC(boshiamy_log, Info)
#define BOSHIAMY_WARN() FCITX_LOGC(boshiamy_log, Warn)

namespace {

constexpr size_t MaxRadicals = 5;
constexpr size_t MaxWildcardResults = 200;

constexpr uint32_t ModifierMask =
    static_cast<uint32_t>(KeyState::Shift) |
    static_cast<uint32_t>(KeyState::Ctrl) |
    static_cast<uint32_t>(KeyState::Alt) |
    static_cast<uint32_t>(KeyState::Super) |
    static_cast<uint32_t>(KeyState::Hyper) |
    static_cast<uint32_t>(KeyState::Super2) |
    static_cast<uint32_t>(KeyState::Hyper2) |
    static_cast<uint32_t>(KeyState::Meta);

constexpr uint32_t ShiftMask = static_cast<uint32_t>(KeyState::Shift);

uint32_t modifiers(const Key &key) {
    return static_cast<uint32_t>(key.states()) & ModifierMask;
}

bool isRadical(char c) {
    return (c >= 'a' && c <= 'z') || c == ',' || c == '.' || c == '\'' ||
           c == '[' || c == ']';
}

// Printable ASCII produced by key, or 0.
char asciiOf(const Key &key) {
    auto uc = Key::keySymToUnicode(key.sym());
    if (uc >= 0x20 && uc < 0x7f) {
        return static_cast<char>(uc);
    }
    return 0;
}

int digitOf(const Key &key) {
    if (modifiers(key)) {
        return -1;
    }
    if (key.sym() >= FcitxKey_0 && key.sym() <= FcitxKey_9) {
        return key.sym() - FcitxKey_0;
    }
    if (key.sym() >= FcitxKey_KP_0 && key.sym() <= FcitxKey_KP_9) {
        return key.sym() - FcitxKey_KP_0;
    }
    return -1;
}

bool isKey(const Key &key, KeySym sym, uint32_t mods = 0) {
    return key.sym() == sym && modifiers(key) == mods;
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

int suffixOffset(char c) {
    switch (c) {
    case 'v':
        return 1;
    case 'r':
        return 2;
    case 's':
        return 3;
    case 'f':
        return 4;
    case 'w':
        return 5;
    default:
        return -1;
    }
}

const std::unordered_set<std::string> &zhuyinInitials() {
    static const std::unordered_set<std::string> set{
        "ㄅ", "ㄆ", "ㄇ", "ㄈ", "ㄉ", "ㄊ", "ㄋ", "ㄌ", "ㄍ", "ㄎ", "ㄏ",
        "ㄐ", "ㄑ", "ㄒ", "ㄓ", "ㄔ", "ㄕ", "ㄖ", "ㄗ", "ㄘ", "ㄙ"};
    return set;
}

const std::unordered_set<std::string> &zhuyinMedials() {
    static const std::unordered_set<std::string> set{"ㄧ", "ㄨ", "ㄩ"};
    return set;
}

const std::unordered_set<std::string> &zhuyinFinals() {
    static const std::unordered_set<std::string> set{
        "ㄚ", "ㄛ", "ㄜ", "ㄝ", "ㄞ", "ㄟ", "ㄠ",
        "ㄡ", "ㄢ", "ㄣ", "ㄤ", "ㄥ", "ㄦ"};
    return set;
}

const std::unordered_set<std::string> &zhuyinTones() {
    static const std::unordered_set<std::string> set{"ˊ", "ˇ", "ˋ", "˙"};
    return set;
}

KeyList selectionKeys() {
    return {Key(FcitxKey_0), Key(FcitxKey_1), Key(FcitxKey_2), Key(FcitxKey_3),
            Key(FcitxKey_4), Key(FcitxKey_5), Key(FcitxKey_6), Key(FcitxKey_7),
            Key(FcitxKey_8), Key(FcitxKey_9)};
}

class BoshiamyCandidateWord : public CandidateWord {
public:
    BoshiamyCandidateWord(BoshiamyEngine *engine, std::string word,
                          std::string comment)
        : CandidateWord(Text(word)), engine_(engine), word_(std::move(word)) {
        if (!comment.empty()) {
            setComment(Text(std::move(comment)));
        }
    }

    void select(InputContext *inputContext) const override {
        auto *state = inputContext->propertyFor(engine_->factory());
        state->commit(word_);
    }

    const std::string &word() const { return word_; }

private:
    BoshiamyEngine *engine_;
    std::string word_;
};

struct QuickCommand {
    std::string_view code;
    BoshiamyTableMode mode;
};

constexpr QuickCommand QuickCommands[] = {
    {",,t", BoshiamyTableMode::Traditional},
    {",,c", BoshiamyTableMode::Simplified},
    {",,ct", BoshiamyTableMode::TradSimp},
    {",,j", BoshiamyTableMode::Japanese},
};

} // namespace

// ---------------------------------------------------------------------------
// BoshiamyState
// ---------------------------------------------------------------------------

void BoshiamyState::reset() {
    mode_ = BoshiamyMode::Radical;
    buffer_.clear();
    aux_.clear();
    candidates_.clear();
    zhuyin_.fill("");
    zhuyinQueried_ = false;
}

void BoshiamyState::commit(const std::string &text) {
    ic_->commitString(text);
    reset();
    // TODO: show the shortest code (簡:) and Zhuyin reading (音:) of the
    // committed character, like UCL_LIU.
    updateUI();
}

void BoshiamyState::keyEvent(KeyEvent &event) {
    if (event.isRelease()) {
        return;
    }
    const auto &key = event.key();
    if (key.isModifier()) {
        return;
    }

    // Clear transient messages (e.g. error hints) on next key.
    if (!aux_.empty() && buffer_.empty() && mode_ == BoshiamyMode::Radical) {
        aux_.clear();
        updateUI();
    }

    if (mode_ == BoshiamyMode::Zhuyin) {
        handleZhuyinKey(event);
        return;
    }
    handleRadicalKey(event);
}

bool BoshiamyState::handleCandidateKeys(KeyEvent &event) {
    const auto &key = event.key();
    auto candidateList = ic_->inputPanel().candidateList();
    if (!candidateList || candidateList->empty()) {
        return false;
    }
    if (int digit = digitOf(key); digit >= 0) {
        event.filterAndAccept();
        if (digit < candidateList->size()) {
            candidateList->candidate(digit).select(ic_);
        }
        return true;
    }
    if (isKey(key, FcitxKey_space, ShiftMask) ||
        isKey(key, FcitxKey_Page_Down) || isKey(key, FcitxKey_KP_Page_Down)) {
        event.filterAndAccept();
        nextPage();
        return true;
    }
    if (isKey(key, FcitxKey_Page_Up) || isKey(key, FcitxKey_KP_Page_Up)) {
        event.filterAndAccept();
        prevPage();
        return true;
    }
    return false;
}

void BoshiamyState::handleRadicalKey(KeyEvent &event) {
    const auto &key = event.key();
    const uint32_t mods = modifiers(key);
    const char ch = asciiOf(key);

    if (buffer_.empty() && mode_ == BoshiamyMode::Radical) {
        // Idle: only plain radical keys start composing.
        if (!mods && isRadical(ch)) {
            buffer_.push_back(ch);
            updateRadicalCandidates();
            event.filterAndAccept();
        }
        return;
    }

    // Composing. Let shortcuts like Ctrl+C go to the application.
    if (mods & ~ShiftMask) {
        return;
    }

    if (isKey(key, FcitxKey_Escape)) {
        reset();
        updateUI();
        event.filterAndAccept();
        return;
    }

    if (isKey(key, FcitxKey_BackSpace)) {
        if (mode_ == BoshiamyMode::Homophone) {
            // Back to the "'<code>" input.
            mode_ = BoshiamyMode::Radical;
        } else if (!buffer_.empty()) {
            buffer_.pop_back();
        }
        if (buffer_.empty()) {
            reset();
            updateUI();
        } else {
            updateRadicalCandidates();
        }
        event.filterAndAccept();
        return;
    }

    if (isKey(key, FcitxKey_Return) || isKey(key, FcitxKey_KP_Enter)) {
        // Commit raw radicals.
        commit(buffer_);
        event.filterAndAccept();
        return;
    }

    if (isKey(key, FcitxKey_space)) {
        handleSpace(event);
        return;
    }

    if (handleCandidateKeys(event)) {
        return;
    }

    if (mode_ == BoshiamyMode::Homophone) {
        if (ch && !mods && isRadical(ch)) {
            // Commit the current homophone, then start a new code.
            auto candidateList = ic_->inputPanel().candidateList();
            if (candidateList && !candidateList->empty()) {
                ic_->commitString(candidateList
                                      ->candidate(std::max(
                                          candidateList->cursorIndex(), 0))
                                      .text()
                                      .toString());
            }
            reset();
            buffer_.push_back(ch);
            updateRadicalCandidates();
            event.filterAndAccept();
            return;
        }
        event.filterAndAccept();
        return;
    }

    // "';" enters Zhuyin lookup mode.
    if (buffer_ == "'" && ch == ';' && !mods) {
        if (engine_->phonetic().loaded()) {
            reset();
            mode_ = BoshiamyMode::Zhuyin;
            updateUI();
        }
        event.filterAndAccept();
        return;
    }

    char radical = 0;
    if (!mods && isRadical(ch)) {
        radical = ch;
    } else if (buffer_.rfind(",,", 0) == 0 && ch >= 'A' && ch <= 'Z') {
        // Allow typing quick commands in upper case, e.g. ",,CT".
        radical = static_cast<char>(std::tolower(ch));
    } else if ((ch == '?' || ch == '*') && *engine_->config().wildcard) {
        radical = ch;
    }

    if (radical) {
        const bool homophonePrefix = buffer_.size() >= 1 && buffer_[0] == '\'';
        const size_t limit = MaxRadicals + (homophonePrefix ? 1 : 0);
        if (buffer_.size() < limit) {
            buffer_.push_back(radical);
            updateRadicalCandidates();
        }
        event.filterAndAccept();
        return;
    }

    if (ch) {
        // Other printable keys: commit the current candidate (or drop the
        // invalid code), then let the key go to the application.
        auto candidateList = ic_->inputPanel().candidateList();
        if (candidateList && !candidateList->empty()) {
            candidateList->candidate(std::max(candidateList->cursorIndex(), 0))
                .select(ic_);
        } else {
            reset();
            updateUI();
        }
        return;
    }

    // Swallow other keys (arrows, Tab, ...) while composing.
    event.filterAndAccept();
}

void BoshiamyState::handleSpace(KeyEvent &event) {
    event.filterAndAccept();
    auto candidateList = ic_->inputPanel().candidateList();

    if (mode_ == BoshiamyMode::Homophone) {
        // Space flips through homophone pages, 0-9 selects.
        nextPage();
        return;
    }

    if (executeQuickCommand()) {
        return;
    }

    if (!candidateList || candidateList->empty()) {
        // Invalid code: clear it, like the official Boshiamy.
        reset();
        updateUI();
        return;
    }

    const auto &candidate =
        candidateList->candidate(std::max(candidateList->cursorIndex(), 0));
    if (buffer_.size() > 1 && buffer_[0] == '\'') {
        enterHomophone(candidate.text().toString());
        return;
    }
    candidate.select(ic_);
}

std::optional<std::string> BoshiamyState::quickCommandHint() const {
    for (const auto &cmd : QuickCommands) {
        if (buffer_ == cmd.code) {
            return std::string(_("Switch to: ")) +
                   BoshiamyEngine::tableModeName(cmd.mode);
        }
    }
    return std::nullopt;
}

bool BoshiamyState::executeQuickCommand() {
    for (const auto &cmd : QuickCommands) {
        if (buffer_ == cmd.code) {
            reset();
            engine_->setTableMode(cmd.mode, ic_);
            if (!engine_->table()) {
                aux_ = std::string(_("Table file not found: ")) +
                       BoshiamyEngine::tableFileName(cmd.mode);
            }
            updateUI();
            return true;
        }
    }
    return false;
}

void BoshiamyState::updateRadicalCandidates() {
    candidates_.clear();
    aux_.clear();
    mode_ = BoshiamyMode::Radical;

    std::string code = buffer_;
    if (code.size() > 1 && code[0] == '\'') {
        code = code.substr(1);
    }

    if (auto hint = quickCommandHint()) {
        aux_ = *hint;
        updateUI();
        return;
    }

    const auto *table = engine_->table();
    if (!table) {
        aux_ = std::string(_("Table file not found: ")) +
               BoshiamyEngine::tableFileName(*engine_->config().tableMode);
        updateUI();
        return;
    }

    if (code.find_first_of("?*") != std::string::npos) {
        for (auto &[word, wordCode] :
             table->wildcard(code, MaxWildcardResults)) {
            candidates_.emplace_back(std::move(word), toUpper(wordCode));
        }
    } else if (const auto *words = table->lookup(code)) {
        for (const auto &word : *words) {
            candidates_.emplace_back(word, "");
        }
    } else if (*engine_->config().dynamicSuffix && code.size() >= 2) {
        // code + V/R/S/F/W selects the 2nd-6th candidate of code.
        int offset = suffixOffset(code.back());
        if (offset > 0) {
            const auto *base = table->lookup(code.substr(0, code.size() - 1));
            if (base && static_cast<int>(base->size()) > offset) {
                candidates_.emplace_back((*base)[offset], "");
            }
        }
    }
    updateUI();
}

void BoshiamyState::enterHomophone(const std::string &word) {
    auto words = engine_->phonetic().homophones(word);
    if (words.empty()) {
        aux_ = std::string(_("No homophones for ")) + word;
        updateUI();
        return;
    }
    mode_ = BoshiamyMode::Homophone;
    aux_ = std::string(_("Homophones of ")) + word;
    candidates_.clear();
    for (auto &w : words) {
        candidates_.emplace_back(std::move(w), "");
    }
    updateUI();
}

bool BoshiamyState::zhuyinEmpty() const {
    return std::all_of(zhuyin_.begin(), zhuyin_.end(),
                       [](const std::string &s) { return s.empty(); });
}

std::string BoshiamyState::zhuyinPreedit() const {
    std::string result;
    for (const auto &s : zhuyin_) {
        result += s;
    }
    return result;
}

std::string BoshiamyState::zhuyinKeys() const {
    std::string keys;
    for (const auto &s : zhuyin_) {
        if (s.empty()) {
            continue;
        }
        if (auto k = engine_->phonetic().keyForSymbol(s)) {
            keys.push_back(*k);
        }
    }
    return keys;
}

void BoshiamyState::queryZhuyin() {
    candidates_.clear();
    aux_.clear();
    if (const auto *chars = engine_->phonetic().lookup(zhuyinKeys())) {
        for (const auto &c : *chars) {
            candidates_.emplace_back(c, "");
        }
    }
    if (candidates_.empty()) {
        aux_ = _("No match");
        zhuyin_[Tone].clear();
        zhuyinQueried_ = false;
    } else {
        zhuyinQueried_ = true;
    }
    updateUI();
}

void BoshiamyState::handleZhuyinKey(KeyEvent &event) {
    const auto &key = event.key();
    const uint32_t mods = modifiers(key);
    if (mods & ~ShiftMask) {
        return;
    }

    if (isKey(key, FcitxKey_Escape)) {
        reset();
        updateUI();
        event.filterAndAccept();
        return;
    }

    if (isKey(key, FcitxKey_BackSpace)) {
        event.filterAndAccept();
        aux_.clear();
        if (zhuyinQueried_) {
            zhuyinQueried_ = false;
            candidates_.clear();
            zhuyin_[Tone].clear();
            updateUI();
            return;
        }
        if (zhuyinEmpty()) {
            // Nothing left to delete, leave Zhuyin mode.
            reset();
            updateUI();
            return;
        }
        for (int i = SlotCount - 1; i >= 0; i--) {
            if (!zhuyin_[i].empty()) {
                zhuyin_[i].clear();
                break;
            }
        }
        updateUI();
        return;
    }

    if (isKey(key, FcitxKey_Return) || isKey(key, FcitxKey_KP_Enter)) {
        reset();
        updateUI();
        event.filterAndAccept();
        return;
    }

    if (zhuyinQueried_) {
        if (isKey(key, FcitxKey_space)) {
            event.filterAndAccept();
            auto candidateList = ic_->inputPanel().candidateList();
            if (candidateList && !candidateList->empty()) {
                candidateList
                    ->candidate(std::max(candidateList->cursorIndex(), 0))
                    .select(ic_);
            }
            return;
        }
        if (!handleCandidateKeys(event)) {
            // The syllable is complete, ignore other keys.
            event.filterAndAccept();
        }
        return;
    }

    if (isKey(key, FcitxKey_space)) {
        event.filterAndAccept();
        if (!zhuyinEmpty()) {
            // First tone.
            queryZhuyin();
        }
        return;
    }

    const char ch = asciiOf(key);
    if (ch && !mods) {
        event.filterAndAccept();
        auto symbol = engine_->phonetic().symbolForKey(ch);
        if (!symbol) {
            return;
        }
        aux_.clear();
        if (zhuyinInitials().count(*symbol)) {
            zhuyin_[Initial] = *symbol;
        } else if (zhuyinMedials().count(*symbol)) {
            zhuyin_[Medial] = *symbol;
        } else if (zhuyinFinals().count(*symbol)) {
            zhuyin_[Final] = *symbol;
        } else if (zhuyinTones().count(*symbol)) {
            if (zhuyinEmpty()) {
                return;
            }
            zhuyin_[Tone] = *symbol;
            queryZhuyin();
            return;
        }
        updateUI();
        return;
    }

    event.filterAndAccept();
}

void BoshiamyState::nextPage() {
    auto candidateList = ic_->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    auto *pageable = candidateList->toPageable();
    if (!pageable) {
        return;
    }
    if (pageable->hasNext()) {
        pageable->next();
    } else if (pageable->totalPages() > 1) {
        pageable->setPage(0);
    } else {
        return;
    }
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void BoshiamyState::prevPage() {
    auto candidateList = ic_->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    auto *pageable = candidateList->toPageable();
    if (!pageable) {
        return;
    }
    if (pageable->hasPrev()) {
        pageable->prev();
    } else if (pageable->totalPages() > 1) {
        pageable->setPage(pageable->totalPages() - 1);
    } else {
        return;
    }
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void BoshiamyState::updateUI() {
    auto &inputPanel = ic_->inputPanel();
    inputPanel.reset();

    std::string preedit;
    std::string aux;
    switch (mode_) {
    case BoshiamyMode::Radical:
    case BoshiamyMode::Homophone:
        preedit = toUpper(buffer_);
        break;
    case BoshiamyMode::Zhuyin:
        aux = _("[Zhuyin] ");
        preedit = zhuyinPreedit();
        break;
    }
    aux += aux_;

    if (!preedit.empty()) {
        Text text(preedit, TextFormatFlag::Underline);
        text.setCursor(preedit.size());
        if (ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
            inputPanel.setClientPreedit(text);
        } else {
            inputPanel.setPreedit(text);
        }
    }
    if (!aux.empty()) {
        inputPanel.setAuxUp(Text(aux));
    }

    if (!candidates_.empty()) {
        auto candidateList = std::make_unique<CommonCandidateList>();
        candidateList->setSelectionKey(selectionKeys());
        candidateList->setPageSize(*engine_->config().pageSize);
        candidateList->setCursorPositionAfterPaging(
            CursorPositionAfterPaging::ResetToFirst);
        for (const auto &[word, comment] : candidates_) {
            candidateList->append<BoshiamyCandidateWord>(engine_, word,
                                                         comment);
        }
        candidateList->setGlobalCursorIndex(0);
        inputPanel.setCandidateList(std::move(candidateList));
    }

    ic_->updatePreedit();
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

// ---------------------------------------------------------------------------
// BoshiamyEngine
// ---------------------------------------------------------------------------

BoshiamyEngine::BoshiamyEngine(Instance *instance)
    : instance_(instance), factory_([this](InputContext &ic) {
          return new BoshiamyState(this, &ic);
      }) {
    reloadConfig();
    instance_->inputContextManager().registerProperty("boshiamyState",
                                                      &factory_);
}

BoshiamyEngine::~BoshiamyEngine() = default;

std::vector<InputMethodEntry> BoshiamyEngine::listInputMethods() {
    std::vector<InputMethodEntry> result;
    result.emplace_back(std::move(
        InputMethodEntry("boshiamy", _("Boshiamy"), "zh_TW", "boshiamy")
            .setLabel("嘸")
            .setIcon("fcitx-boshiamy")
            .setConfigurable(true)));
    return result;
}

const char *BoshiamyEngine::tableFileName(BoshiamyTableMode mode) {
    switch (mode) {
    case BoshiamyTableMode::Traditional:
        return "boshiamy-t.txt";
    case BoshiamyTableMode::Simplified:
        return "boshiamy-c.txt";
    case BoshiamyTableMode::TradSimp:
        return "boshiamy-ct.txt";
    case BoshiamyTableMode::Japanese:
        return "boshiamy-j.txt";
    }
    return "boshiamy-t.txt";
}

const char *BoshiamyEngine::tableModeLabel(BoshiamyTableMode mode) {
    switch (mode) {
    case BoshiamyTableMode::Traditional:
        return "嘸";
    case BoshiamyTableMode::Simplified:
        return "簡";
    case BoshiamyTableMode::TradSimp:
        return "台";
    case BoshiamyTableMode::Japanese:
        return "日";
    }
    return "嘸";
}

std::string BoshiamyEngine::tableModeName(BoshiamyTableMode mode) {
    return BoshiamyTableModeI18NAnnotation::toString(mode);
}

const BoshiamyTable *BoshiamyEngine::table() {
    const auto mode = *config_.tableMode;
    const auto idx = static_cast<size_t>(mode);
    if (!tables_[idx] && !tableTried_[idx]) {
        tableTried_[idx] = true;
        const auto &paths = StandardPaths::global();
        const auto file = std::string("boshiamy/") + tableFileName(mode);
        const auto path = paths.locate(StandardPathsType::PkgData, file);
        auto table = std::make_unique<BoshiamyTable>();
        if (path.empty() || !table->load(path.string())) {
            BOSHIAMY_WARN() << "Failed to load Boshiamy table " << file;
            return nullptr;
        }
        // Optional user table, merged into every table.
        const auto custom =
            paths.locate(StandardPathsType::PkgData, "boshiamy/custom.txt");
        if (!custom.empty()) {
            table->merge(custom.string());
        }
        BOSHIAMY_INFO() << "Loaded Boshiamy table " << path << " with "
                        << table->size() << " entries";
        tables_[idx] = std::move(table);
    }
    return tables_[idx].get();
}

const BoshiamyPhonetic &BoshiamyEngine::phonetic() {
    if (!phoneticTried_) {
        phoneticTried_ = true;
        const auto path = StandardPaths::global().locate(
            StandardPathsType::PkgData, "boshiamy/pinyi.txt");
        if (path.empty() || !phonetic_.load(path.string())) {
            BOSHIAMY_WARN() << "Failed to load boshiamy/pinyi.txt";
        }
    }
    return phonetic_;
}

void BoshiamyEngine::reloadDictionaries() {
    for (auto &table : tables_) {
        table.reset();
    }
    tableTried_.fill(false);
    phoneticTried_ = false;
}

void BoshiamyEngine::setTableMode(BoshiamyTableMode mode, InputContext *ic) {
    config_.tableMode.setValue(mode);
    safeSaveAsIni(config_, ConfPath);
    if (ic) {
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    }
}

void BoshiamyEngine::reloadConfig() {
    readAsIni(config_, ConfPath);
    reloadDictionaries();
}

void BoshiamyEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    safeSaveAsIni(config_, ConfPath);
    reloadConfig();
    if (auto *ic = instance_->mostRecentInputContext()) {
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    }
}

void BoshiamyEngine::activate(const InputMethodEntry & /*entry*/,
                              InputContextEvent &event) {
    auto *ic = event.inputContext();
    // chttrans (OpenCC) and fullwidth hook into the commit filter.
    chttrans();
    std::vector<const char *> actions{"chttrans"};
    if (*config_.useFullWidth && fullwidth()) {
        actions.push_back("fullwidth");
    }
    for (const auto *name : actions) {
        if (auto *action =
                instance_->userInterfaceManager().lookupAction(name)) {
            ic->statusArea().addAction(StatusGroup::InputMethod, action);
        }
    }
    // Preload the table so the first key press is fast.
    table();
}

void BoshiamyEngine::deactivate(const InputMethodEntry &entry,
                                InputContextEvent &event) {
    reset(entry, event);
}

void BoshiamyEngine::reset(const InputMethodEntry & /*entry*/,
                           InputContextEvent &event) {
    auto *state = event.inputContext()->propertyFor(&factory_);
    state->reset();
    state->updateUI();
}

void BoshiamyEngine::keyEvent(const InputMethodEntry & /*entry*/,
                              KeyEvent &keyEvent) {
    auto *state = keyEvent.inputContext()->propertyFor(&factory_);
    state->keyEvent(keyEvent);
}

std::string BoshiamyEngine::subMode(const InputMethodEntry & /*entry*/,
                                    InputContext & /*ic*/) {
    return tableModeName(*config_.tableMode);
}

std::string BoshiamyEngine::subModeLabelImpl(const InputMethodEntry & /*entry*/,
                                             InputContext & /*ic*/) {
    return tableModeLabel(*config_.tableMode);
}

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(boshiamy, fcitx::BoshiamyEngineFactory);
