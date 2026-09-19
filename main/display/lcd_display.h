#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "image_block.h"
#include "lvgl_display.h"
#include "search/search_memory_budget.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* app_name_label_ = nullptr;
    lv_obj_t* brand_tag_label_ = nullptr;
    lv_obj_t* subbar_left_label_ = nullptr;
    lv_obj_t* search_query_label_ = nullptr;
    lv_obj_t* search_detail_label_ = nullptr;
    lv_obj_t* search_content_panel_ = nullptr;
    lv_obj_t* search_accent_bar_ = nullptr;
    lv_obj_t* search_title_label_ = nullptr;
    lv_obj_t* idle_view_ = nullptr;
    lv_obj_t* idle_octagon_ = nullptr;
    lv_obj_t* idle_octagon_inner_ = nullptr;
    lv_obj_t* idle_ready_label_ = nullptr;
    lv_obj_t* idle_version_label_ = nullptr;
    lv_obj_t* idle_prompt_card_ = nullptr;
    lv_obj_t* idle_prompt_main_label_ = nullptr;
    lv_obj_t* idle_prompt_desc_label_ = nullptr;
    lv_obj_t* voice_view_ = nullptr;
    lv_obj_t* telemetry_label_ = nullptr;
    lv_obj_t* hud_frame_ = nullptr;
    lv_obj_t* button_hint_bar_ = nullptr;
    lv_obj_t* footer_left_key_label_ = nullptr;
    lv_obj_t* footer_left_text_label_ = nullptr;
    lv_obj_t* footer_right_key_label_ = nullptr;
    lv_obj_t* footer_right_text_label_ = nullptr;
    lv_obj_t* settings_menu_overlay_ = nullptr;
    std::vector<std::string> text_input_keys_;
    std::size_t text_input_selected_ = 0;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* search_result_image_bubble_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    alignas(4)
        std::array<uint8_t, search_memory_budget::kLcdLineBufferBytes> search_image_line_buffer_{};
    bool search_image_stream_active_ = false;
    size_t search_image_source_width_ = 0;
    size_t search_image_source_height_ = 0;
    size_t search_image_dest_x_ = 0;
    size_t search_image_dest_y_ = 0;
    size_t search_image_dest_width_ = 0;
    size_t search_image_dest_height_ = 0;
    bool search_image_flip_x_ = false;
    bool search_image_flip_y_ = false;
    bool panel_mirror_x_ = false;
    bool panel_mirror_y_ = false;
    bool panel_swap_xy_ = false;
    bool search_image_panel_rotated_ = false;
    // Bumped by BeginSearchImageTransition; streamed blocks carrying an older
    // token are rejected instead of being painted with stale geometry.
    uint32_t search_image_stream_epoch_ = 0;

    enum class CyberPage {
        Home,
        Connecting,
        Activation,
        Listening,
        Search,
        Settings,
        WifiList,
        Keyboard,
        Error,
    };
    CyberPage cyber_page_ = CyberPage::Home;

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetSearchContent(const char* query, const char* detail) override;
    virtual void ResetSearchContent() override;
    virtual void ShowSettingsMenu(const std::vector<std::string>& items, std::size_t selected,
                                  const char* title, const char* hint) override;
    virtual void ShowTextInput(const char* title, const char* value, bool masked,
                               const std::vector<std::string>& keys, std::size_t selected,
                               TextInputPage page, const char* status, const char* hint,
                               const char* context) override;
    virtual void HideSettingsMenu() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    // Displays a result image without the normal short preview timeout.
    // When full_screen is true, the image is shown without the search-page
    // chrome and is scaled to fit the complete LCD.
    void SetSearchResultImage(std::unique_ptr<LvglImage> image, bool full_screen = false);
    // Configure source-image mirroring before starting a streamed image. The
    // panel is rotated only for the hidden-UI full-screen image transaction and
    // restored before the normal UI is shown again.
    void SetSearchImageTransform(bool flip_x, bool flip_y);
    // Consume one transient RGB565 big-endian JPEG block. The block is copied
    // or sent before this method returns and may then be reused by the decoder.
    // stream_epoch must match the token returned by BeginSearchImageTransition;
    // blocks from superseded downloads are rejected so they cannot repaint the
    // panel with the previous image's geometry.
    bool DrawSearchResultImageBlock(const Rgb565ImageBlock& block, uint32_t stream_epoch);
    // Finish a streamed full-screen image. On failure, restore the search page.
    void EndSearchResultImageStream(bool success);
    // Prepare the panel for the next streamed result. When a full-screen
    // image is already shown, keep its layout and panel rotation so switching
    // results paints over the current frame instead of flashing a restored
    // home-page frame in between. Returns the epoch token that the following
    // DrawSearchResultImageBlock calls must carry.
    uint32_t BeginSearchImageTransition();
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

private:
    void SetCyberHudLocked(const char* tag, const char* title, const char* telemetry,
                           const char* mode, uint32_t accent_rgb);
    void SetCyberFooterLocked(const char* left_key, const char* left_text, const char* right_key,
                              const char* right_text, uint32_t accent_rgb);
    void SetCyberTelemetryLocked(const char* text);
    void SetCyberAccentLocked(uint32_t accent_rgb);
    void HideCyberBodyViewsLocked();
    void ShowCyberHomeLocked();
    void ShowCyberListeningLocked();
    void ShowCyberMessageLocked(const char* title, const char* query, const char* detail,
                                uint32_t accent_rgb);
    void SetPreviewImageLocked(std::unique_ptr<LvglImage> image, bool full_screen,
                               bool keep_visible);
    void SetButtonHintLocked(const char* text, lv_coord_t height, bool wrap);
    void RestoreHomeButtonHintLocked();
    void RestoreSearchPageLayoutLocked();
    void ApplyFullScreenSearchLayoutLocked();
    bool SetSearchImagePanelRotationLocked(bool rotate_90);
    bool BeginSearchResultImageStreamLocked(size_t image_width, size_t image_height);
    // Fills the whole panel with the theme background color through the
    // current (possibly rotated) panel mapping. Used to erase frames that
    // were written directly to the panel and that LVGL would not repaint.
    bool FillScreenWithThemeBackgroundLocked();
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
