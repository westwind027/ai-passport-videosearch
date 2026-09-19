#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }

private:
    std::string name_;
};

enum class TextInputPage {
    Lower,
    Upper,
    Symbols,
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    // Render the voice-search content in the main page body. The default
    // implementation keeps non-LCD displays source-compatible.
    virtual void SetSearchContent(const char* query, const char* detail) {}
    // Reset the voice-search body explicitly. Keeping this separate from
    // ClearChatMessages prevents unrelated late chat/state callbacks from
    // erasing a search result that is still on screen.
    virtual void ResetSearchContent() {}
    virtual void ShowSettingsMenu(const std::vector<std::string>& items, std::size_t selected,
                                  const char* title = "设置",
                                  const char* hint = "上/下选择  OK进入  长按返回") {}
    // Render a character input page. The caller owns the keyboard state and
    // supplies the visible keys so the same control can edit Wi-Fi passwords
    // and the search service URL.
    virtual void ShowTextInput(const char* title, const char* value, bool masked,
                               const std::vector<std::string>& keys, std::size_t selected,
                               TextInputPage page, const char* status = "",
                               const char* hint = "长按上下调节", const char* context = "") {}
    virtual void HideSettingsMenu() {}
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    DisplayLockGuard(Display* display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() { display_->Unlock(); }

private:
    Display* display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
