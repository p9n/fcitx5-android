/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright 2026 Fcitx5 for Android Contributors
 */
#ifndef FCITX5_ANDROID_BOSHIAMY_ENGINE_H
#define FCITX5_ANDROID_BOSHIAMY_ENGINE_H

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "dictionary.h"

namespace fcitx {

/**
 * Boshiamy radical tables, matching the official table switching commands.
 */
enum class BoshiamyTableMode {
    Traditional, // ,,T  liu-uni.tab
    Simplified,  // ,,C  liu-uni2.tab
    TradSimp,    // ,,CT liu-uni3.tab
    Japanese,    // ,,J  liu-uni4.tab
};

FCITX_CONFIG_ENUM_NAME_WITH_I18N(BoshiamyTableMode,
                                 N_("Traditional Chinese (,,T)"),
                                 N_("Simplified Chinese (,,C)"),
                                 N_("Traditional and Simplified (,,CT)"),
                                 N_("Japanese (,,J)"));

FCITX_CONFIGURATION(
    BoshiamyConfig,
    OptionWithAnnotation<BoshiamyTableMode, BoshiamyTableModeI18NAnnotation>
        tableMode{this, "TableMode", _("Table"),
                  BoshiamyTableMode::Traditional};
    Option<int, IntConstrain> pageSize{this, "PageSize", _("Page size"), 10,
                                       IntConstrain(3, 10)};
    Option<bool> dynamicSuffix{
        this, "DynamicSuffix",
        _("Select 2nd-6th candidate with V/R/S/F/W suffix"), true};
    Option<bool> wildcard{this, "Wildcard",
                          _("Enable wildcard lookup with ? and *"), true};
    Option<bool> useFullWidth{this, "UseFullWidth",
                              _("Show full width toggle"), true};
    // TODO: Show shortest code (簡:) and Zhuyin reading (音:) hints after
    // committing a character, like UCL_LIU.
);

enum class BoshiamyMode {
    // Typing radicals, buffer_ holds the radical keys.
    Radical,
    // Homophone candidates of the character typed with "'<code>".
    Homophone,
    // Zhuyin lookup, entered with "';".
    Zhuyin,
};

class BoshiamyEngine;

class BoshiamyState : public InputContextProperty {
public:
    BoshiamyState(BoshiamyEngine *engine, InputContext *ic)
        : engine_(engine), ic_(ic) {}

    void reset();
    void keyEvent(KeyEvent &event);
    void commit(const std::string &text);
    void updateUI();

private:
    // Zhuyin syllable slots.
    enum ZhuyinSlot { Initial = 0, Medial, Final, Tone, SlotCount };

    void handleRadicalKey(KeyEvent &event);
    void handleZhuyinKey(KeyEvent &event);
    bool handleCandidateKeys(KeyEvent &event);
    void handleSpace(KeyEvent &event);
    bool executeQuickCommand();
    std::optional<std::string> quickCommandHint() const;

    void updateRadicalCandidates();
    void enterHomophone(const std::string &word);
    void queryZhuyin();
    std::string zhuyinPreedit() const;
    std::string zhuyinKeys() const;
    bool zhuyinEmpty() const;

    void nextPage();
    void prevPage();

    BoshiamyEngine *engine_;
    InputContext *ic_;
    BoshiamyMode mode_ = BoshiamyMode::Radical;
    std::string buffer_;
    std::string aux_;
    // (word, comment) pairs shown in the candidate list.
    std::vector<std::pair<std::string, std::string>> candidates_;
    std::array<std::string, SlotCount> zhuyin_;
    bool zhuyinQueried_ = false;
};

class BoshiamyEngine final : public InputMethodEngineV2 {
public:
    explicit BoshiamyEngine(Instance *instance);
    ~BoshiamyEngine() override;

    Instance *instance() { return instance_; }

    std::vector<InputMethodEntry> listInputMethods() override;
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;
    void activate(const InputMethodEntry &entry,
                  InputContextEvent &event) override;
    void deactivate(const InputMethodEntry &entry,
                    InputContextEvent &event) override;
    void reset(const InputMethodEntry &entry,
               InputContextEvent &event) override;
    std::string subMode(const InputMethodEntry &entry,
                        InputContext &ic) override;
    std::string subModeLabelImpl(const InputMethodEntry &entry,
                                 InputContext &ic) override;

    void reloadConfig() override;
    const Configuration *getConfig() const override { return &config_; }
    void setConfig(const RawConfig &config) override;

    const BoshiamyConfig &config() const { return config_; }
    auto *factory() { return &factory_; }

    // Active radical table, loaded on demand. May return nullptr if the
    // table file is missing.
    const BoshiamyTable *table();
    const BoshiamyPhonetic &phonetic();
    void setTableMode(BoshiamyTableMode mode, InputContext *ic);
    void reloadDictionaries();

    static const char *tableFileName(BoshiamyTableMode mode);
    static const char *tableModeLabel(BoshiamyTableMode mode);
    static std::string tableModeName(BoshiamyTableMode mode);

    FCITX_ADDON_DEPENDENCY_LOADER(chttrans, instance_->addonManager());
    FCITX_ADDON_DEPENDENCY_LOADER(fullwidth, instance_->addonManager());

private:
    static constexpr const char *ConfPath = "conf/boshiamy.conf";

    Instance *instance_;
    BoshiamyConfig config_;
    std::array<std::unique_ptr<BoshiamyTable>, 4> tables_;
    std::array<bool, 4> tableTried_{};
    BoshiamyPhonetic phonetic_;
    bool phoneticTried_ = false;
    FactoryFor<BoshiamyState> factory_;
};

class BoshiamyEngineFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new BoshiamyEngine(manager->instance());
    }
};

} // namespace fcitx

#endif // FCITX5_ANDROID_BOSHIAMY_ENGINE_H
