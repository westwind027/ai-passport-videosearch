#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

namespace {

constexpr size_t kSearchImageMaxLinePixels = 320;
static_assert(search_memory_budget::kLcdLineBufferBytes ==
                  kSearchImageMaxLinePixels * sizeof(uint16_t),
              "the fixed LCD line workspace must fit one panel row");

constexpr uint32_t kCyberYellow = 0xFCEE0A;
constexpr uint32_t kCyberCyan = 0x00F0FF;
constexpr uint32_t kCyberRed = 0xFF003C;
constexpr uint32_t kCyberGreen = 0x00FF66;
constexpr uint32_t kCyberBackground = 0x090B10;
constexpr uint32_t kCyberTopBackground = 0x0A0E17;
constexpr uint32_t kCyberSubBackground = 0x0D121C;
constexpr uint32_t kCyberCard = 0x121622;
constexpr uint32_t kCyberCardSelected = 0x1A2538;
constexpr uint32_t kCyberBorder = 0x1E2638;
constexpr uint32_t kCyberText = 0xF0F4FC;
constexpr uint32_t kCyberMuted = 0x5E738C;

constexpr lv_coord_t kCyberTopHeight = 28;
constexpr lv_coord_t kCyberSubHeight = 18;
constexpr lv_coord_t kCyberBodyTop = kCyberTopHeight + kCyberSubHeight;
constexpr lv_coord_t kCyberFooterHeight = 28;
constexpr lv_coord_t kCyberTelemetryHeight = 18;
constexpr lv_coord_t kCyberFooterTop = 320 - kCyberFooterHeight;
constexpr lv_coord_t kCyberTelemetryTop = kCyberFooterTop - kCyberTelemetryHeight;

const lv_point_precise_t kCyberOctagonPoints[] = {
    {26, 0}, {78, 0}, {103, 25}, {103, 78}, {78, 103}, {26, 103}, {0, 78}, {0, 25}, {26, 0},
};
const lv_point_precise_t kCyberInnerOctagonPoints[] = {
    {24, 0}, {68, 0}, {91, 23}, {91, 68}, {68, 91}, {24, 91}, {0, 68}, {0, 23}, {24, 0},
};

struct CyberPanelStyleEntry {
    bool initialized = false;
    uint32_t background_rgb = 0;
    uint32_t border_rgb = 0;
    lv_coord_t border_width = 0;
    lv_coord_t radius = 0;
    lv_style_t style{};
};

struct CyberLabelStyleEntry {
    bool initialized = false;
    const lv_font_t* font = nullptr;
    uint32_t color_rgb = 0;
    lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT;
    lv_style_t style{};
};

// Local styles allocate and repeatedly grow a property array on every object.
// The cyber UI uses a small palette, so cache immutable style combinations and
// let each object keep only one style reference.
std::array<CyberPanelStyleEntry, 12> g_cyber_panel_styles;
std::array<CyberLabelStyleEntry, 16> g_cyber_label_styles;

lv_style_t* GetCyberPanelStyle(uint32_t background_rgb, uint32_t border_rgb,
                               lv_coord_t border_width, lv_coord_t radius) {
    for (auto& entry : g_cyber_panel_styles) {
        if (entry.initialized && entry.background_rgb == background_rgb &&
            entry.border_rgb == border_rgb && entry.border_width == border_width &&
            entry.radius == radius) {
            return &entry.style;
        }
    }
    for (auto& entry : g_cyber_panel_styles) {
        if (!entry.initialized) {
            entry.initialized = true;
            entry.background_rgb = background_rgb;
            entry.border_rgb = border_rgb;
            entry.border_width = border_width;
            entry.radius = radius;
            lv_style_init(&entry.style);
            lv_style_set_radius(&entry.style, radius);
            lv_style_set_bg_opa(&entry.style, LV_OPA_COVER);
            lv_style_set_bg_color(&entry.style, lv_color_hex(background_rgb));
            lv_style_set_border_width(&entry.style, border_width);
            lv_style_set_border_color(&entry.style, lv_color_hex(border_rgb));
            lv_style_set_pad_all(&entry.style, 0);
            return &entry.style;
        }
    }
    return nullptr;
}

lv_style_t* GetCyberLabelStyle(const lv_font_t* font, uint32_t color_rgb,
                               lv_text_align_t alignment) {
    for (auto& entry : g_cyber_label_styles) {
        if (entry.initialized && entry.font == font && entry.color_rgb == color_rgb &&
            entry.alignment == alignment) {
            return &entry.style;
        }
    }
    for (auto& entry : g_cyber_label_styles) {
        if (!entry.initialized) {
            entry.initialized = true;
            entry.font = font;
            entry.color_rgb = color_rgb;
            entry.alignment = alignment;
            lv_style_init(&entry.style);
            lv_style_set_text_font(&entry.style, font);
            lv_style_set_text_color(&entry.style, lv_color_hex(color_rgb));
            lv_style_set_text_align(&entry.style, alignment);
            return &entry.style;
        }
    }
    return nullptr;
}

void StyleCyberPanel(lv_obj_t* object, uint32_t background_rgb, uint32_t border_rgb,
                     lv_coord_t border_width = 1, lv_coord_t radius = 0) {
    lv_style_t* style = GetCyberPanelStyle(background_rgb, border_rgb, border_width, radius);
    if (style != nullptr) {
        lv_obj_add_style(object, style, 0);
    } else {
        lv_obj_set_style_radius(object, radius, 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(background_rgb), 0);
        lv_obj_set_style_border_width(object, border_width, 0);
        lv_obj_set_style_border_color(object, lv_color_hex(border_rgb), 0);
        lv_obj_set_style_pad_all(object, 0, 0);
    }
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

void StyleCyberLabel(lv_obj_t* label, const lv_font_t* font, uint32_t color_rgb,
                     lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT) {
    lv_style_t* style = GetCyberLabelStyle(font, color_rgb, alignment);
    if (style != nullptr) {
        lv_obj_add_style(label, style, 0);
    } else {
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color_rgb), 0);
        lv_obj_set_style_text_align(label, alignment, 0);
    }
}

void DrawCyberHudFrame(lv_event_t* event) {
    lv_obj_t* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.base.layer = layer;
    line.color = lv_obj_get_style_line_color(object, LV_PART_MAIN);
    line.width = 2;

    constexpr lv_coord_t kLeft = 6;
    constexpr lv_coord_t kRight = 233;
    constexpr lv_coord_t kTop = 48;
    const lv_coord_t kBottom = kCyberTelemetryTop - 4;
    constexpr lv_coord_t kLength = 8;
    const lv_point_precise_t segments[][2] = {
        {{kLeft, kTop}, {kLeft + kLength, kTop}},
        {{kLeft, kTop}, {kLeft, kTop + kLength}},
        {{kRight - kLength, kTop}, {kRight, kTop}},
        {{kRight, kTop}, {kRight, kTop + kLength}},
        {{kLeft, kBottom}, {kLeft + kLength, kBottom}},
        {{kLeft, kBottom - kLength}, {kLeft, kBottom}},
        {{kRight - kLength, kBottom}, {kRight, kBottom}},
        {{kRight, kBottom - kLength}, {kRight, kBottom}},
    };
    for (const auto& segment : segments) {
        line.p1 = {segment[0].x + area.x1, segment[0].y + area.y1};
        line.p2 = {segment[1].x + area.x1, segment[1].y + area.y1};
        lv_draw_line(layer, &line);
    }
}

void DrawCyberVoiceSpectrum(lv_event_t* event) {
    lv_obj_t* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);

    lv_draw_rect_dsc_t rectangle;
    lv_draw_rect_dsc_init(&rectangle);
    rectangle.base.layer = layer;
    rectangle.bg_opa = LV_OPA_COVER;
    rectangle.bg_color = lv_color_hex(kCyberRed);
    rectangle.radius = 1;
    constexpr lv_coord_t kVoiceHeights[] = {12, 26, 46, 32, 50, 38, 22, 42, 16};
    for (std::size_t index = 0; index < std::size(kVoiceHeights); ++index) {
        const lv_coord_t x = area.x1 + 71 + static_cast<lv_coord_t>(index) * 10;
        const lv_coord_t y = area.y1 + 54 - kVoiceHeights[index];
        const lv_area_t bar = {x, y, x + 4, y + kVoiceHeights[index] - 1};
        lv_draw_rect(layer, &rectangle, &bar);
    }
}

}  // namespace

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_material_symbols_16_4);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // Both persisted theme names resolve to the same fixed cyber palette on
    // this single-purpose product. This prevents an old "light" NVS value
    // from flashing the retired white UI during boot.
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(kCyberBackground));
    light_theme->set_text_color(lv_color_hex(kCyberText));
    light_theme->set_chat_background_color(lv_color_hex(kCyberCard));
    light_theme->set_user_bubble_color(lv_color_hex(kCyberCyan));
    light_theme->set_assistant_bubble_color(lv_color_hex(kCyberCard));
    light_theme->set_system_bubble_color(lv_color_hex(kCyberTopBackground));
    light_theme->set_system_text_color(lv_color_hex(kCyberText));
    light_theme->set_border_color(lv_color_hex(kCyberBorder));
    light_theme->set_low_battery_color(lv_color_hex(kCyberRed));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(kCyberBackground));
    dark_theme->set_text_color(lv_color_hex(kCyberText));
    dark_theme->set_chat_background_color(lv_color_hex(kCyberCard));
    dark_theme->set_user_bubble_color(lv_color_hex(kCyberCyan));
    dark_theme->set_assistant_bubble_color(lv_color_hex(kCyberCard));
    dark_theme->set_system_bubble_color(lv_color_hex(kCyberTopBackground));
    dark_theme->set_system_text_color(lv_color_hex(kCyberText));
    dark_theme->set_border_color(lv_color_hex(kCyberBorder));
    dark_theme->set_low_battery_color(lv_color_hex(kCyberRed));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    panel_mirror_x_ = mirror_x;
    panel_mirror_y_ = mirror_y;
    panel_swap_xy_ = swap_xy;

    // Paint the panel in the product background color before LVGL starts, so
    // power-on never flashes the retired white XiaoZhi screen. The panel API
    // consumes RGB565 in big-endian byte order.
    std::vector<uint8_t> buffer(static_cast<size_t>(width_) * 2);
    for (int x = 0; x < width_; ++x) {
        buffer[static_cast<size_t>(x) * 2] = 0x08;
        buffer[static_cast<size_t>(x) * 2 + 1] = 0x42;
    }
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    panel_mirror_x_ = mirror_x;
    panel_mirror_y_ = mirror_y;
    panel_swap_xy_ = swap_xy;

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    panel_mirror_x_ = mirror_x;
    panel_mirror_y_ = mirror_y;
    panel_swap_xy_ = swap_xy;

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetSearchResultImage(nullptr);

    if (settings_menu_overlay_ != nullptr) {
        lv_obj_del(settings_menu_overlay_);
        settings_menu_overlay_ = nullptr;
    }

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (search_query_label_ != nullptr) {
        lv_obj_del(search_query_label_);
    }
    if (search_detail_label_ != nullptr) {
        lv_obj_del(search_detail_label_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (button_hint_bar_ != nullptr) {
        lv_obj_del(button_hint_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE && !CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    app_name_label_ = lv_label_create(top_bar_);
    lv_label_set_text(app_name_label_, "语音搜索");
    lv_label_set_long_mode(app_name_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(app_name_label_, 86);
    lv_obj_set_style_text_font(app_name_label_, text_font, 0);
    lv_obj_set_style_text_color(app_name_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    time_label_ = lv_label_create(right_icons);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_set_style_text_font(time_label_, text_font, 0);
    lv_obj_set_style_text_color(time_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_right(time_label_, lvgl_theme->spacing(2), 0);

    network_label_ = lv_label_create(right_icons);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.55);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.55);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    // The startup frame is the same search shell as the post-activation home
    // page. Network/activation progress is reported through the same status
    // label by Application after the shell is ready.
    lv_label_set_text(status_label_, Lang::Strings::STANDBY);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    // Keep only one image bubble. Search result navigation calls this method
    // repeatedly, and retaining every decoded RGB565 image would exhaust the
    // C3 heap after a few result changes.
    if (search_result_image_bubble_ != nullptr) {
        lv_obj_del(search_result_image_bubble_);
        search_result_image_bubble_ = nullptr;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    search_result_image_bubble_ = img_bubble;
    lv_obj_add_event_cb(
        img_bubble,
        [](lv_event_t* e) {
            auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (display != nullptr &&
                display->search_result_image_bubble_ == lv_event_get_current_target(e)) {
                display->search_result_image_bubble_ = nullptr;
            }
        },
        LV_EVENT_DELETE, this);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ - 24, 132);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Tactical top header: fixed 28 px, matching the HTML prototype. */
    top_bar_ = lv_obj_create(screen);
    const lv_coord_t top_bar_height = kCyberTopHeight;
    lv_obj_set_size(top_bar_, LV_HOR_RES, top_bar_height);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_hex(kCyberTopBackground), 0);
    lv_obj_set_style_border_width(top_bar_, 1, 0);
    lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top_bar_, lv_color_hex(0x1A2538), 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    const lv_font_t* header_text_font = &font_noto_sans_basic_16_4;
    const lv_font_t* header_icon_font = &font_material_symbols_16_4;
    constexpr lv_coord_t kHeaderPadding = 6;
    constexpr lv_coord_t kHeaderAppWidth = 80;
    constexpr lv_coord_t kHeaderTimeWidth = 48;
    constexpr lv_coord_t kHeaderIconWidth = 18;
    constexpr lv_coord_t kHeaderGap = 3;
    constexpr lv_coord_t kHeaderRightWidth =
        kHeaderTimeWidth + kHeaderIconWidth * 2 + kHeaderGap * 2;
    const lv_coord_t header_right_x = LV_HOR_RES - kHeaderPadding - kHeaderRightWidth;
    const lv_coord_t header_text_y = (top_bar_height - header_text_font->line_height) / 2;
    const lv_coord_t header_icon_y = (top_bar_height - header_icon_font->line_height) / 2;

    brand_tag_label_ = lv_label_create(top_bar_);
    lv_label_set_text(brand_tag_label_, "AI");
    StyleCyberPanel(brand_tag_label_, kCyberYellow, kCyberYellow, 0, 2);
    StyleCyberLabel(brand_tag_label_, &font_noto_sans_basic_14_1, 0x000000, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(brand_tag_label_, 28, 16);
    lv_obj_set_pos(brand_tag_label_, kHeaderPadding, 6);

    app_name_label_ = lv_label_create(top_bar_);
    lv_label_set_text(app_name_label_, "语义搜索");
    lv_label_set_long_mode(app_name_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_size(app_name_label_, kHeaderAppWidth, text_font->line_height);
    lv_obj_set_pos(app_name_label_, 40, (top_bar_height - text_font->line_height) / 2);
    lv_obj_set_style_text_font(app_name_label_, text_font, 0);
    lv_obj_set_style_text_color(app_name_label_, lv_color_hex(kCyberText), 0);

    // Right status slots: time, Wi-Fi and battery.
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, kHeaderRightWidth, top_bar_height);
    lv_obj_set_pos(right_icons, header_right_x, 0);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_style_layout(right_icons, LV_LAYOUT_NONE, 0);

    network_label_ = lv_label_create(right_icons);
    lv_label_set_text(network_label_, "");
    lv_obj_set_size(network_label_, kHeaderIconWidth, header_icon_font->line_height);
    lv_obj_set_pos(network_label_, kHeaderTimeWidth + kHeaderGap, header_icon_y);
    lv_obj_set_style_text_font(network_label_, header_icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(kCyberCyan), 0);
    lv_obj_set_style_text_align(network_label_, LV_TEXT_ALIGN_CENTER, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_size(battery_label_, kHeaderIconWidth, header_icon_font->line_height);
    lv_obj_set_pos(battery_label_, kHeaderTimeWidth + kHeaderIconWidth + kHeaderGap * 2,
                   header_icon_y);
    lv_obj_set_style_text_font(battery_label_, header_icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(kCyberGreen), 0);
    lv_obj_set_style_text_align(battery_label_, LV_TEXT_ALIGN_CENTER, 0);

    time_label_ = lv_label_create(right_icons);
    lv_label_set_text(time_label_, "--:--");
    lv_label_set_long_mode(time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(time_label_, kHeaderTimeWidth, header_text_font->line_height);
    lv_obj_set_pos(time_label_, 0, header_text_y);
    lv_obj_set_style_text_font(time_label_, header_text_font, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(kCyberText), 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_RIGHT, 0);

    /* Tactical telemetry sub-header: 18 px at y=28. */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, kCyberSubHeight);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_hex(kCyberSubBackground), 0);
    lv_obj_set_style_border_width(status_bar_, 1, 0);
    lv_obj_set_style_border_side(status_bar_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(status_bar_, lv_color_hex(0x1E2B40), 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, top_bar_height);

    subbar_left_label_ = lv_label_create(status_bar_);
    lv_label_set_text(subbar_left_label_, "DEVICE READY");
    lv_label_set_long_mode(subbar_left_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_size(subbar_left_label_, 112, kCyberSubHeight);
    lv_obj_set_pos(subbar_left_label_, 8, 0);
    StyleCyberLabel(subbar_left_label_, &font_noto_sans_basic_14_1, kCyberMuted);
    lv_obj_set_style_pad_top(subbar_left_label_, 1, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_size(notification_label_, 112, kCyberSubHeight);
    lv_obj_set_pos(notification_label_, 120, 0);
    StyleCyberLabel(notification_label_, &font_noto_sans_basic_14_1, kCyberYellow,
                    LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(notification_label_, "");
    lv_obj_set_style_pad_top(notification_label_, 1, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_size(status_label_, 112, kCyberSubHeight);
    lv_obj_set_pos(status_label_, 120, 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    StyleCyberLabel(status_label_, &font_noto_sans_basic_14_1, kCyberCyan, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(status_label_, "SYS_STANDBY");
    lv_obj_set_style_pad_top(status_label_, 1, 0);

    /* One draw object replaces four heap-backed corner widgets. */
    hud_frame_ = lv_obj_create(screen);
    lv_obj_set_size(hud_frame_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(hud_frame_, 0, 0);
    lv_obj_set_style_bg_opa(hud_frame_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hud_frame_, 0, 0);
    lv_obj_set_style_pad_all(hud_frame_, 0, 0);
    lv_obj_set_style_line_color(hud_frame_, lv_color_hex(kCyberCyan), 0);
    lv_obj_remove_flag(hud_frame_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hud_frame_, DrawCyberHudFrame, LV_EVENT_DRAW_MAIN_END, nullptr);

    /* Idle neural core and interaction prompt. */
    idle_view_ = lv_obj_create(screen);
    lv_obj_set_size(idle_view_, LV_HOR_RES, kCyberTelemetryTop - kCyberBodyTop);
    lv_obj_set_pos(idle_view_, 0, kCyberBodyTop);
    lv_obj_set_style_bg_opa(idle_view_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_view_, 0, 0);
    lv_obj_set_style_pad_all(idle_view_, 0, 0);
    lv_obj_set_scrollbar_mode(idle_view_, LV_SCROLLBAR_MODE_OFF);

    idle_octagon_ = lv_line_create(idle_view_);
    lv_line_set_points(idle_octagon_, kCyberOctagonPoints, std::size(kCyberOctagonPoints));
    lv_obj_set_pos(idle_octagon_, 68, 10);
    lv_obj_set_style_line_width(idle_octagon_, 1, 0);
    lv_obj_set_style_line_color(idle_octagon_, lv_color_hex(kCyberCyan), 0);

    idle_octagon_inner_ = lv_line_create(idle_view_);
    lv_line_set_points(idle_octagon_inner_, kCyberInnerOctagonPoints,
                       std::size(kCyberInnerOctagonPoints));
    lv_obj_set_pos(idle_octagon_inner_, 74, 16);
    lv_obj_set_style_line_width(idle_octagon_inner_, 1, 0);
    lv_obj_set_style_line_color(idle_octagon_inner_, lv_color_hex(0x164552), 0);

    idle_ready_label_ = lv_label_create(idle_view_);
    lv_label_set_text(idle_ready_label_, "READY");
    lv_obj_set_size(idle_ready_label_, 104, 26);
    lv_obj_set_pos(idle_ready_label_, 68, 46);
    StyleCyberLabel(idle_ready_label_, &font_noto_sans_basic_20_4, kCyberText,
                    LV_TEXT_ALIGN_CENTER);

    idle_version_label_ = lv_label_create(idle_view_);
    lv_label_set_text(idle_version_label_, "VOICE SEARCH");
    lv_obj_set_size(idle_version_label_, 104, 18);
    lv_obj_set_pos(idle_version_label_, 68, 70);
    StyleCyberLabel(idle_version_label_, &font_noto_sans_basic_14_1, kCyberYellow,
                    LV_TEXT_ALIGN_CENTER);

    idle_prompt_card_ = lv_obj_create(idle_view_);
    lv_obj_set_size(idle_prompt_card_, 216, 62);
    lv_obj_set_pos(idle_prompt_card_, 12, 128);
    StyleCyberPanel(idle_prompt_card_, kCyberCard, 0x1A2538, 1, 2);
    lv_obj_set_style_border_side(idle_prompt_card_, LV_BORDER_SIDE_FULL, 0);

    lv_obj_t* prompt_accent = lv_obj_create(idle_prompt_card_);
    lv_obj_set_size(prompt_accent, 3, 62);
    lv_obj_set_pos(prompt_accent, 0, 0);
    StyleCyberPanel(prompt_accent, kCyberYellow, kCyberYellow, 0);

    lv_obj_t* prompt_tag = lv_label_create(idle_prompt_card_);
    lv_label_set_text(prompt_tag, "INTERACTION PROMPT");
    lv_obj_set_pos(prompt_tag, 10, 3);
    lv_obj_set_size(prompt_tag, 194, 16);
    StyleCyberLabel(prompt_tag, &font_noto_sans_basic_14_1, kCyberYellow);

    idle_prompt_main_label_ = lv_label_create(idle_prompt_card_);
    lv_label_set_text(idle_prompt_main_label_, "按下 OK 开始录音");
    lv_obj_set_pos(idle_prompt_main_label_, 10, 20);
    lv_obj_set_size(idle_prompt_main_label_, 194, 18);
    StyleCyberLabel(idle_prompt_main_label_, text_font, kCyberText);

    idle_prompt_desc_label_ = lv_label_create(idle_prompt_card_);
    lv_label_set_text(idle_prompt_desc_label_, "说出要查找的视频场景或画面");
    lv_obj_set_pos(idle_prompt_desc_label_, 10, 40);
    lv_obj_set_size(idle_prompt_desc_label_, 194, 18);
    StyleCyberLabel(idle_prompt_desc_label_, text_font, 0x728CA6);

    /* Shared search/system message card. */
    search_content_panel_ = lv_obj_create(screen);
    lv_obj_set_size(search_content_panel_, 220, 118);
    lv_obj_set_pos(search_content_panel_, 10, 94);
    StyleCyberPanel(search_content_panel_, kCyberCard, 0x233147, 1, 2);

    search_accent_bar_ = lv_obj_create(search_content_panel_);
    lv_obj_set_size(search_accent_bar_, 3, 118);
    lv_obj_set_pos(search_accent_bar_, 0, 0);
    StyleCyberPanel(search_accent_bar_, kCyberCyan, kCyberCyan, 0);

    search_title_label_ = lv_label_create(search_content_panel_);
    lv_label_set_text(search_title_label_, "SEMANTIC SEARCH");
    lv_obj_set_pos(search_title_label_, 10, 7);
    lv_obj_set_size(search_title_label_, 196, 16);
    StyleCyberLabel(search_title_label_, &font_noto_sans_basic_14_1, kCyberCyan);

    search_query_label_ = lv_label_create(search_content_panel_);
    lv_obj_set_pos(search_query_label_, 10, 28);
    lv_obj_set_size(search_query_label_, 196, text_font->line_height * 2);
    lv_obj_set_style_text_font(search_query_label_, text_font, 0);
    lv_label_set_long_mode(search_query_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(search_query_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(search_query_label_, lv_color_hex(kCyberText), 0);

    search_detail_label_ = lv_label_create(search_content_panel_);
    lv_obj_set_pos(search_detail_label_, 10, 72);
    lv_obj_set_size(search_detail_label_, 196, text_font->line_height * 2);
    lv_obj_set_style_text_font(search_detail_label_, text_font, 0);
    lv_label_set_long_mode(search_detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(search_detail_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(search_detail_label_, lv_color_hex(0x6A839E), 0);
    lv_label_set_text(search_query_label_, "");
    lv_label_set_text(search_detail_label_, "");
    lv_obj_add_flag(search_content_panel_, LV_OBJ_FLAG_HIDDEN);

    /* Recording view: the spectrum is painted in one draw callback. */
    voice_view_ = lv_obj_create(screen);
    lv_obj_set_size(voice_view_, 220, 70);
    lv_obj_set_pos(voice_view_, 10, 58);
    lv_obj_set_style_bg_opa(voice_view_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(voice_view_, 0, 0);
    lv_obj_set_style_pad_all(voice_view_, 0, 0);
    lv_obj_add_event_cb(voice_view_, DrawCyberVoiceSpectrum, LV_EVENT_DRAW_MAIN_END, nullptr);
    lv_obj_add_flag(voice_view_, LV_OBJ_FLAG_HIDDEN);

    telemetry_label_ = lv_label_create(screen);
    lv_label_set_text(telemetry_label_, "WIFI READY");
    lv_obj_set_pos(telemetry_label_, 12, kCyberTelemetryTop);
    lv_obj_set_size(telemetry_label_, 216, kCyberTelemetryHeight);
    lv_label_set_long_mode(telemetry_label_, LV_LABEL_LONG_DOT);
    StyleCyberLabel(telemetry_label_, &font_noto_sans_basic_14_1, 0x3F556E, LV_TEXT_ALIGN_CENTER);

    /* Tactical two-cell key footer. */
    const lv_coord_t button_hint_height = kCyberFooterHeight;
    button_hint_bar_ = lv_obj_create(screen);
    lv_obj_set_size(button_hint_bar_, LV_HOR_RES, button_hint_height);
    lv_obj_set_style_radius(button_hint_bar_, 0, 0);
    lv_obj_set_style_bg_color(button_hint_bar_, lv_color_hex(0x0A0D16), 0);
    lv_obj_set_style_bg_opa(button_hint_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button_hint_bar_, 1, 0);
    lv_obj_set_style_border_side(button_hint_bar_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(button_hint_bar_, lv_color_hex(0x1A2538), 0);
    lv_obj_set_style_pad_all(button_hint_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(button_hint_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(button_hint_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto create_footer_cell = [&](lv_coord_t x, lv_obj_t*& key_label, lv_obj_t*& text_label) {
        key_label = lv_label_create(button_hint_bar_);
        lv_obj_set_size(key_label, 34, 18);
        lv_obj_set_pos(key_label, x + 7, 5);
        StyleCyberPanel(key_label, 0x131C2B, 0x283A54, 1, 1);
        StyleCyberLabel(key_label, &font_noto_sans_basic_14_1, kCyberText, LV_TEXT_ALIGN_CENTER);

        text_label = lv_label_create(button_hint_bar_);
        lv_obj_set_size(text_label, 76, 18);
        lv_obj_set_pos(text_label, x + 44, 5);
        lv_label_set_long_mode(text_label, LV_LABEL_LONG_DOT);
        StyleCyberLabel(text_label, text_font, 0x7B94AD);
    };
    create_footer_cell(0, footer_left_key_label_, footer_left_text_label_);
    create_footer_cell(120, footer_right_key_label_, footer_right_text_label_);
    lv_obj_set_style_border_width(footer_right_text_label_, 1, 0);
    lv_obj_set_style_border_side(footer_right_text_label_, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(footer_right_text_label_, lv_color_hex(0x141C2B), 0);

    SetCyberFooterLocked("OK", "开启录音", "HOLD", "设置", kCyberYellow);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -kCyberFooterHeight - 4);
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "UI ready: heap free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void LcdDisplay::SetCyberAccentLocked(uint32_t accent_rgb) {
    const lv_color_t accent = lv_color_hex(accent_rgb);
    if (hud_frame_ != nullptr) {
        lv_obj_set_style_line_color(hud_frame_, accent, 0);
        lv_obj_invalidate(hud_frame_);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_color(status_label_, accent, 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, accent, 0);
    }
}

void LcdDisplay::SetCyberHudLocked(const char* tag, const char* title, const char* telemetry,
                                   const char* mode, uint32_t accent_rgb) {
    if (brand_tag_label_ != nullptr) {
        lv_label_set_text(brand_tag_label_, tag != nullptr ? tag : "AI");
        lv_obj_set_style_text_color(
            brand_tag_label_, lv_color_hex(accent_rgb == kCyberRed ? kCyberText : 0x000000), 0);
    }
    if (brand_tag_label_ != nullptr) {
        lv_obj_set_style_bg_color(brand_tag_label_, lv_color_hex(accent_rgb), 0);
        lv_obj_set_style_border_color(brand_tag_label_, lv_color_hex(accent_rgb), 0);
    }
    if (app_name_label_ != nullptr) {
        lv_label_set_text(app_name_label_, title != nullptr ? title : "语义搜索");
    }
    if (subbar_left_label_ != nullptr) {
        lv_label_set_text(subbar_left_label_, telemetry != nullptr ? telemetry : "DEVICE READY");
    }
    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, mode != nullptr ? mode : "SYS_STANDBY");
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    SetCyberAccentLocked(accent_rgb);
}

void LcdDisplay::SetCyberFooterLocked(const char* left_key, const char* left_text,
                                      const char* right_key, const char* right_text,
                                      uint32_t accent_rgb) {
    if (footer_left_key_label_ == nullptr || footer_left_text_label_ == nullptr ||
        footer_right_key_label_ == nullptr || footer_right_text_label_ == nullptr) {
        return;
    }

    lv_label_set_text(footer_left_key_label_, left_key != nullptr ? left_key : "");
    lv_label_set_text(footer_left_text_label_, left_text != nullptr ? left_text : "");
    lv_label_set_text(footer_right_key_label_, right_key != nullptr ? right_key : "");
    lv_label_set_text(footer_right_text_label_, right_text != nullptr ? right_text : "");

    lv_obj_set_style_bg_color(footer_left_key_label_, lv_color_hex(0x131C2B), 0);
    lv_obj_set_style_border_color(footer_left_key_label_, lv_color_hex(0x283A54), 0);
    lv_obj_set_style_text_color(footer_left_key_label_, lv_color_hex(kCyberText), 0);
    lv_obj_set_style_bg_color(footer_right_key_label_, lv_color_hex(accent_rgb), 0);
    lv_obj_set_style_border_color(footer_right_key_label_, lv_color_hex(accent_rgb), 0);
    lv_obj_set_style_text_color(footer_right_key_label_,
                                lv_color_hex(accent_rgb == kCyberRed ? kCyberText : 0x000000), 0);
    lv_obj_remove_flag(button_hint_bar_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetCyberTelemetryLocked(const char* text) {
    if (telemetry_label_ == nullptr) {
        return;
    }
    const bool ascii =
        text == nullptr || std::all_of(text, text + std::strlen(text), [](char value) {
            return static_cast<unsigned char>(value) < 0x80;
        });
    if (ascii) {
        lv_obj_set_style_text_font(telemetry_label_, &font_noto_sans_basic_14_1, 0);
        lv_obj_set_style_text_color(telemetry_label_, lv_color_hex(0x3F556E), 0);
    } else if (current_theme_ != nullptr) {
        // Keep the caller's accent color for error messages while switching
        // Chinese text to the full theme font.
        const auto* theme = static_cast<LvglTheme*>(current_theme_);
        lv_obj_set_style_text_font(telemetry_label_, theme->text_font()->font(), 0);
    }
    lv_label_set_text(telemetry_label_, text != nullptr ? text : "");
    lv_obj_remove_flag(telemetry_label_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::HideCyberBodyViewsLocked() {
    if (idle_view_ != nullptr) {
        lv_obj_add_flag(idle_view_, LV_OBJ_FLAG_HIDDEN);
    }
    if (voice_view_ != nullptr) {
        lv_obj_add_flag(voice_view_, LV_OBJ_FLAG_HIDDEN);
    }
    if (search_content_panel_ != nullptr) {
        lv_obj_add_flag(search_content_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ShowCyberHomeLocked() {
    cyber_page_ = CyberPage::Home;
    HideCyberBodyViewsLocked();
    if (idle_view_ != nullptr) {
        lv_obj_remove_flag(idle_view_, LV_OBJ_FLAG_HIDDEN);
    }
    SetCyberHudLocked("AI", "语义搜索", "DEVICE READY", "SYS_STANDBY", kCyberCyan);
    SetCyberTelemetryLocked("WIFI READY");
    SetCyberFooterLocked("OK", "开启录音", "HOLD", "设置", kCyberYellow);
}

void LcdDisplay::ShowCyberListeningLocked() {
    cyber_page_ = CyberPage::Listening;
    HideCyberBodyViewsLocked();
    if (voice_view_ != nullptr) {
        lv_obj_remove_flag(voice_view_, LV_OBJ_FLAG_HIDDEN);
    }
    if (search_content_panel_ != nullptr) {
        lv_obj_set_pos(search_content_panel_, 10, 134);
        lv_obj_remove_flag(search_content_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(search_title_label_, "VOICE CAPTURE ACTIVE");
    lv_label_set_text(search_query_label_, "正在录音中...");
    lv_label_set_text(search_detail_label_, "按 OK 结束录音并发送检索");
    lv_obj_set_style_border_color(search_content_panel_, lv_color_hex(kCyberRed), 0);
    lv_obj_set_style_text_color(search_title_label_, lv_color_hex(kCyberRed), 0);
    SetCyberHudLocked("REC", "语音识别", "VOICE INPUT", "SAMPLING...", kCyberRed);
    SetCyberTelemetryLocked("VOICE INPUT ACTIVE");
    SetCyberFooterLocked("OK", "结束录音", "U/D", "取消", kCyberRed);
}

void LcdDisplay::ShowCyberMessageLocked(const char* title, const char* query, const char* detail,
                                        uint32_t accent_rgb) {
    HideCyberBodyViewsLocked();
    if (search_content_panel_ == nullptr) {
        return;
    }
    lv_obj_set_pos(search_content_panel_, 10, 94);
    lv_obj_set_style_border_color(search_content_panel_, lv_color_hex(accent_rgb), 0);
    if (search_accent_bar_ != nullptr) {
        lv_obj_set_style_bg_color(search_accent_bar_, lv_color_hex(accent_rgb), 0);
        lv_obj_set_style_border_color(search_accent_bar_, lv_color_hex(accent_rgb), 0);
    }
    lv_label_set_text(search_title_label_, title != nullptr ? title : "SEMANTIC SEARCH");
    lv_obj_set_style_text_color(search_title_label_, lv_color_hex(accent_rgb), 0);
    lv_label_set_text(search_query_label_, query != nullptr ? query : "");
    lv_label_set_text(search_detail_label_, detail != nullptr ? detail : "");
    lv_obj_remove_flag(search_content_panel_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
    if (!setup_ui_called_ || status == nullptr || status_bar_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        ShowCyberHomeLocked();
    } else if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        ShowCyberListeningLocked();
    } else if (strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               strstr(status, "扫描") != nullptr || strstr(status, "连接中") != nullptr) {
        cyber_page_ = CyberPage::Connecting;
        SetCyberHudLocked("NET", "网络链路", "WIFI LINK", "CONNECTING...", kCyberYellow);
        ShowCyberMessageLocked("NETWORK HANDSHAKE", "", "正在建立安全链路...", kCyberYellow);
        SetCyberTelemetryLocked("CONNECTING WIFI");
        SetCyberFooterLocked("OK", "中止", "HOLD", "重选", kCyberYellow);
    } else if (strcmp(status, Lang::Strings::ACTIVATION) == 0 ||
               strstr(status, "激活") != nullptr || strstr(status, "登录") != nullptr ||
               strstr(status, "版本") != nullptr) {
        cyber_page_ = CyberPage::Activation;
        SetCyberHudLocked("PIN", "小智对码", "PAIRING", "WAIT_BINDING", kCyberCyan);
        ShowCyberMessageLocked("DEVICE PAIRING", "", "等待服务器认证...", kCyberCyan);
        SetCyberTelemetryLocked("WAITING FOR PAIRING");
        SetCyberFooterLocked("OK", "刷新状态", "HOLD", "返回", kCyberCyan);
    } else if (strstr(status, "设置") != nullptr || strstr(status, "音量") != nullptr) {
        cyber_page_ = CyberPage::Settings;
        SetCyberHudLocked("CFG", "控制台", "SETTINGS", "4 ITEMS", kCyberYellow);
    } else if (strstr(status, "Wi-Fi") != nullptr || strstr(status, "WIFI") != nullptr) {
        cyber_page_ = CyberPage::WifiList;
        SetCyberHudLocked("AP", "Wi-Fi 列表", "WIFI LIST", "AP SCAN", kCyberYellow);
    } else if (strstr(status, "输入") != nullptr || strstr(status, "编辑") != nullptr) {
        cyber_page_ = CyberPage::Keyboard;
        SetCyberHudLocked("KEY", "密码输入", "TEXT INPUT", "EDITING", kCyberCyan);
    } else if (strstr(status, "搜索失败") != nullptr || strstr(status, "错误") != nullptr) {
        cyber_page_ = CyberPage::Error;
        SetCyberHudLocked("ERR", "检索异常", "SEARCH ERROR", "FAILED", kCyberRed);
        SetCyberFooterLocked("OK", "重新录音", "HOLD", "设置", kCyberRed);
    } else if (strstr(status, "搜索") != nullptr) {
        cyber_page_ = CyberPage::Search;
        const bool complete = strstr(status, "完成") != nullptr;
        SetCyberHudLocked("SRC", "语义检索", "VIDEO SEARCH",
                          complete ? "RESULT_READY" : "SEARCHING...",
                          complete ? kCyberCyan : kCyberYellow);
        SetCyberTelemetryLocked("VIDEO SEARCH · MAX 12");
        SetCyberFooterLocked("U/D", "结果翻页", "HOLD", "全屏", kCyberYellow);
    }
}

void LcdDisplay::RestoreSearchPageLayoutLocked() {
    if (top_bar_ != nullptr) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (telemetry_label_ != nullptr) {
        lv_obj_remove_flag(telemetry_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (hud_frame_ != nullptr) {
        lv_obj_remove_flag(hud_frame_, LV_OBJ_FLAG_HIDDEN);
    }
    if (button_hint_bar_ != nullptr) {
        lv_obj_remove_flag(button_hint_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (cyber_page_ == CyberPage::Home) {
        ShowCyberHomeLocked();
    } else if (cyber_page_ == CyberPage::Listening) {
        ShowCyberListeningLocked();
    } else {
        SetCyberFooterLocked("U/D", "结果翻页", "HOLD", "全屏", kCyberYellow);
    }
    if (preview_image_ != nullptr) {
        lv_obj_set_size(preview_image_, width_ - 24, 132);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 40);
    }
}

void LcdDisplay::SetButtonHintLocked(const char* text, lv_coord_t height, bool wrap) {
    if (button_hint_bar_ == nullptr) {
        return;
    }

    (void)height;
    (void)wrap;
    const bool adjusts = text != nullptr && strstr(text, "调节") != nullptr;
    SetCyberFooterLocked("U/D", adjusts ? "调节" : "选择", "OK", "进入/返回", kCyberYellow);
}

void LcdDisplay::RestoreHomeButtonHintLocked() {
    if (current_theme_ == nullptr) {
        return;
    }
    ShowCyberHomeLocked();
}

void LcdDisplay::ApplyFullScreenSearchLayoutLocked() {
    if (top_bar_ != nullptr) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    HideCyberBodyViewsLocked();
    if (telemetry_label_ != nullptr) {
        lv_obj_add_flag(telemetry_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (hud_frame_ != nullptr) {
        lv_obj_add_flag(hud_frame_, LV_OBJ_FLAG_HIDDEN);
    }
    if (button_hint_bar_ != nullptr) {
        lv_obj_add_flag(button_hint_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_set_size(preview_image_, width_, height_);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    }
}

void LcdDisplay::SetPreviewImageLocked(std::unique_ptr<LvglImage> image, bool full_screen,
                                       bool keep_visible) {
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        search_image_stream_active_ = false;
        const bool was_rotated = search_image_panel_rotated_;
        SetSearchImagePanelRotationLocked(false);
        esp_timer_stop(preview_timer_);
        if (emoji_box_ != nullptr) {
            if (search_query_label_ != nullptr) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        RestoreSearchPageLayoutLocked();
        if (was_rotated) {
            // The streamed frame was written through the rotated panel
            // mapping. LVGL only repaints the regions it considers dirty, so
            // erase the whole panel once and mark everything dirty to avoid
            // leaving a ghost of the full-screen image on the home page.
            FillScreenWithThemeBackgroundLocked();
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(display_);
        }
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    search_image_stream_active_ = false;
    if (full_screen) {
        ApplyFullScreenSearchLayoutLocked();
    } else {
        RestoreSearchPageLayoutLocked();
    }
    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        const lv_coord_t max_width = full_screen ? width_ : width_ - 28;
        const lv_coord_t max_height = full_screen ? height_ : 126;
        const lv_coord_t zoom_w = (max_width * 256) / img_dsc->header.w;
        const lv_coord_t zoom_h = (max_height * 256) / img_dsc->header.h;
        lv_image_set_scale(preview_image_, std::min(zoom_w, zoom_h));
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    if (!keep_visible) {
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
    }
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    SetPreviewImageLocked(std::move(image), false, false);
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    // This build is the standalone video-search application. The XiaoZhi
    // conversation bubble is intentionally not part of its UI; STT is shown
    // by SetSearchContent() and assistant replies are ignored.
    (void)role;
    (void)content;
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::ShowSettingsMenu(const std::vector<std::string>& items, std::size_t selected,
                                  const char* title_text, const char* hint_text) {
    DisplayLockGuard lock(this);

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font = theme->text_font()->font();
    const lv_coord_t screen_width = static_cast<lv_coord_t>(width_);
    (void)hint_text;
    const bool wifi_list = cyber_page_ == CyberPage::WifiList;
    const bool volume_page = title_text != nullptr && strstr(title_text, "音量") != nullptr;
    constexpr std::size_t kMaxVisibleRows = 6;
    const std::size_t row_count = std::min(items.size(), kMaxVisibleRows);
    constexpr lv_coord_t row_height = 30;
    constexpr lv_coord_t row_gap = 3;
    constexpr lv_coord_t first_row_y = 24;
    const std::size_t safe_selected = items.empty() ? 0 : std::min(selected, items.size() - 1);
    const std::size_t first_item =
        items.size() <= kMaxVisibleRows
            ? 0
            : std::min(safe_selected >= kMaxVisibleRows ? safe_selected - kMaxVisibleRows + 1 : 0,
                       items.size() - kMaxVisibleRows);

    if (settings_menu_overlay_ == nullptr) {
        settings_menu_overlay_ = lv_obj_create(lv_screen_active());
        StyleCyberPanel(settings_menu_overlay_, kCyberBackground, kCyberBackground, 0);
    }

    lv_obj_set_size(settings_menu_overlay_, screen_width, kCyberFooterTop - kCyberBodyTop);
    lv_obj_set_pos(settings_menu_overlay_, 0, kCyberBodyTop);
    lv_obj_clean(settings_menu_overlay_);
    lv_obj_remove_flag(settings_menu_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_menu_overlay_);

    lv_obj_t* section_title = lv_label_create(settings_menu_overlay_);
    lv_label_set_text(section_title, wifi_list ? "ACCESS POINTS"
                                               : (volume_page ? "AUDIO LEVEL" : "PARAM CONTROL"));
    lv_obj_set_pos(section_title, 8, 3);
    lv_obj_set_size(section_title, 116, 16);
    StyleCyberLabel(section_title, &font_noto_sans_basic_14_1, kCyberCyan);

    lv_obj_t* section_state = lv_label_create(settings_menu_overlay_);
    lv_label_set_text(section_state, wifi_list ? "RSSI DESC" : "NVS: ACTIVE");
    lv_obj_set_pos(section_state, 124, 3);
    lv_obj_set_size(section_state, 108, 16);
    StyleCyberLabel(section_state, &font_noto_sans_basic_14_1, 0x526D8A, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t* section_rule = lv_obj_create(settings_menu_overlay_);
    lv_obj_set_size(section_rule, 224, 1);
    lv_obj_set_pos(section_rule, 8, 20);
    StyleCyberPanel(section_rule, 0x162438, 0x162438, 0);

    if (wifi_list) {
        char mode[24];
        snprintf(mode, sizeof(mode), "%u APs FOUND", static_cast<unsigned>(items.size()));
        SetCyberHudLocked("AP", "Wi-Fi 列表", "WIFI LIST", mode, kCyberYellow);
        SetCyberTelemetryLocked("SORTED BY SIGNAL");
        SetCyberFooterLocked("U/D", "选择 AP", "OK", "输入密码", kCyberYellow);
    } else if (volume_page) {
        SetCyberHudLocked("VOL", "音量控制", "AUDIO OUTPUT", "LEVEL SET", kCyberYellow);
        SetCyberTelemetryLocked("VOLUME SAVED");
        SetCyberFooterLocked("U/D", "调节", "OK", "确认/返回", kCyberYellow);
    } else {
        SetCyberHudLocked("CFG", "控制台", "SETTINGS", "4 ITEMS", kCyberYellow);
        SetCyberTelemetryLocked("DEVICE SETTINGS");
        SetCyberFooterLocked("U/D", "切换", "OK", "进入/返回", kCyberYellow);
    }

    lv_obj_move_foreground(top_bar_);
    lv_obj_move_foreground(status_bar_);
    lv_obj_move_foreground(telemetry_label_);
    lv_obj_move_foreground(button_hint_bar_);
    if (hud_frame_ != nullptr) {
        lv_obj_move_foreground(hud_frame_);
    }

    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
        const std::size_t item_index = first_item + row_index;
        const bool active = item_index == safe_selected;
        lv_obj_t* row = lv_obj_create(settings_menu_overlay_);
        lv_obj_set_size(row, screen_width - 16, row_height);
        lv_obj_set_pos(row, 8,
                       first_row_y + static_cast<lv_coord_t>(row_index * (row_height + row_gap)));
        StyleCyberPanel(row, active ? kCyberCardSelected : kCyberCard,
                        active ? kCyberYellow : 0x192538, 1, 2);
        if (active) {
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        }

        std::string item_name = items[item_index];
        std::string item_value;
        const std::size_t value_separator = item_name.rfind("  ");
        if (value_separator != std::string::npos) {
            item_value = item_name.substr(value_separator + 2);
            item_name.resize(value_separator);
        } else if (!wifi_list && item_name.rfind("音量 ", 0) == 0) {
            item_value = item_name.substr(std::strlen("音量 ")) + "%";
            item_name = "音量控制";
        } else if (!wifi_list && item_name == "返回主页面") {
            item_value = "ESC";
        }
        if (active) {
            item_name = ">> " + item_name;
        }

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, item_name.c_str());
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_size(label, item_value.empty() ? 204 : 150, row_height);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(active ? kCyberText : 0xCAD8E6), 0);
        lv_obj_set_style_pad_top(label, 7, 0);
        lv_obj_set_pos(label, 8, 0);

        if (!item_value.empty()) {
            lv_obj_t* value_label = lv_label_create(row);
            lv_label_set_text(value_label, item_value.c_str());
            lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
            lv_obj_set_size(value_label, 56, row_height);
            lv_obj_set_pos(value_label, 158, 0);
            StyleCyberLabel(value_label, &font_noto_sans_basic_14_1,
                            active ? kCyberYellow : 0x738EA8, LV_TEXT_ALIGN_RIGHT);
            lv_obj_set_style_pad_top(value_label, 7, 0);
        }
    }
}

void LcdDisplay::HideSettingsMenu() {
    DisplayLockGuard lock(this);
    if (settings_menu_overlay_ != nullptr) {
        lv_obj_delete(settings_menu_overlay_);
        settings_menu_overlay_ = nullptr;
    }
    text_input_keys_.clear();
    text_input_keys_.shrink_to_fit();
    RestoreHomeButtonHintLocked();
    RestoreSearchPageLayoutLocked();
}

void LcdDisplay::SetSearchContent(const char* query, const char* detail) {
    if (search_query_label_ == nullptr || search_detail_label_ == nullptr) {
        // The legacy WeChat layout has no fixed search body; retain a readable
        // fallback there instead of silently dropping the ASR result.
        if (query != nullptr && query[0] != '\0') {
            SetChatMessage("user", query);
        } else if (detail != nullptr) {
            SetChatMessage("system", detail);
        }
        return;
    }

    DisplayLockGuard lock(this);
    const bool has_query = query != nullptr && query[0] != '\0';
    const bool home_prompt = !has_query && (detail == nullptr || detail[0] == '\0' ||
                                            strstr(detail, "开始录音") != nullptr);
    if (home_prompt) {
        ShowCyberHomeLocked();
        return;
    }

    std::string query_text;
    if (has_query) {
        query_text = "“";
        query_text += query;
        query_text += "”";
    }
    const bool error = detail != nullptr &&
                       (strstr(detail, "失败") != nullptr || strstr(detail, "错误") != nullptr ||
                        strstr(detail, "内存") != nullptr || strstr(detail, "smaller") != nullptr);
    cyber_page_ = error ? CyberPage::Error : CyberPage::Search;
    ShowCyberMessageLocked(error ? "SEARCH PIPELINE ERROR" : "SEMANTIC SEARCH", query_text.c_str(),
                           detail != nullptr ? detail : "", error ? kCyberRed : kCyberCyan);
    SetCyberHudLocked(error ? "ERR" : "SRC", error ? "检索异常" : "语义检索",
                      error ? "SEARCH ERROR" : "VIDEO SEARCH", error ? "FAILED" : "PROCESSING...",
                      error ? kCyberRed : kCyberCyan);
    SetCyberTelemetryLocked("VIDEO SEARCH · MAX 12");
    SetCyberFooterLocked("U/D", "结果翻页", "HOLD", "全屏", error ? kCyberRed : kCyberYellow);
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ResetSearchContent() {
    // Search content has an explicit lifecycle. Do not put this in
    // ClearChatMessages(), because a delayed XiaoZhi/state callback may clear
    // chat after a search has already produced a result.
    SetSearchResultImage(nullptr);
    SetSearchContent("", "按下 OK 开始录音\n说出你要查找的视频场景或画面内容");
}

void LcdDisplay::ShowTextInput(const char* title_text, const char* value, bool masked,
                               const std::vector<std::string>& keys, std::size_t selected,
                               TextInputPage page, const char* status_text, const char* hint_text,
                               const char* context_text) {
    DisplayLockGuard lock(this);

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font = theme->text_font()->font();
    const lv_coord_t screen_width = static_cast<lv_coord_t>(width_);
    (void)hint_text;

    if (settings_menu_overlay_ == nullptr) {
        settings_menu_overlay_ = lv_obj_create(lv_screen_active());
        StyleCyberPanel(settings_menu_overlay_, kCyberBackground, kCyberBackground, 0);
    }
    lv_obj_set_size(settings_menu_overlay_, screen_width, kCyberFooterTop - kCyberBodyTop);
    lv_obj_set_pos(settings_menu_overlay_, 0, kCyberBodyTop);
    lv_obj_clean(settings_menu_overlay_);
    lv_obj_remove_flag(settings_menu_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_menu_overlay_);
    lv_obj_move_foreground(top_bar_);
    lv_obj_move_foreground(status_bar_);
    lv_obj_move_foreground(telemetry_label_);
    lv_obj_move_foreground(button_hint_bar_);
    if (hud_frame_ != nullptr) {
        lv_obj_move_foreground(hud_frame_);
    }

    const bool password_input = masked || (context_text != nullptr && context_text[0] != '\0');
    cyber_page_ = CyberPage::Keyboard;
    SetCyberHudLocked(password_input ? "KEY" : "SVR", password_input ? "密码输入" : "服务地址",
                      password_input ? "PASSWORD EDIT" : "SERVICE URL", "EDITING", kCyberCyan);

    lv_obj_t* title = lv_label_create(settings_menu_overlay_);
    lv_label_set_text(title, password_input ? "PASSWORD TERMINAL" : "REMOTE REST URL");
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 8, 1);
    lv_obj_set_size(title, 132, 16);
    StyleCyberLabel(title, &font_noto_sans_basic_14_1, kCyberYellow);

    lv_obj_t* context_label = lv_label_create(settings_menu_overlay_);
    std::string context = context_text != nullptr && context_text[0] != '\0'
                              ? std::string("SSID: ") + context_text
                              : (title_text != nullptr ? title_text : "INPUT");
    lv_label_set_text(context_label, context.c_str());
    lv_label_set_long_mode(context_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(context_label, 140, 1);
    lv_obj_set_size(context_label, 92, 16);
    StyleCyberLabel(context_label, text_font, kCyberCyan, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t* value_box = lv_obj_create(settings_menu_overlay_);
    std::string visible_value;
    if (masked && value != nullptr) {
        visible_value.assign(std::strlen(value), '*');
    } else if (value != nullptr) {
        visible_value = value;
    }
    if (visible_value.empty()) {
        visible_value = "（空）";
    }
    const bool value_is_ascii =
        std::all_of(visible_value.begin(), visible_value.end(),
                    [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
    const lv_font_t* value_font = value_is_ascii ? &font_noto_sans_basic_14_1 : text_font;
    constexpr lv_coord_t value_box_height = 24;
    constexpr lv_coord_t value_box_y = 20;
    lv_obj_set_size(value_box, screen_width - 12, value_box_height);
    lv_obj_set_pos(value_box, 6, value_box_y);
    StyleCyberPanel(value_box, 0x070A12, kCyberCyan, 1, 0);

    lv_obj_t* value_label = lv_label_create(value_box);
    lv_label_set_text(value_label, visible_value.c_str());
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(value_label, screen_width - 26, value_box_height);
    lv_obj_set_style_text_font(value_label, value_font, 0);
    lv_obj_set_style_text_color(value_label, lv_color_hex(kCyberText), 0);
    lv_obj_set_style_pad_top(value_label, 3, 0);
    lv_obj_align(value_label, LV_ALIGN_LEFT_MID, 6, 0);

    const std::size_t safe_selected = keys.empty() ? 0 : selected % keys.size();
    text_input_keys_.assign(keys.begin(), keys.begin() + std::min<std::size_t>(keys.size(), 34));
    text_input_selected_ = safe_selected;
    constexpr lv_coord_t kKeyWidth = 34;
    constexpr lv_coord_t kKeyHeight = 22;
    constexpr lv_coord_t kKeyGap = 4;
    constexpr lv_coord_t kFirstX = 8;
    const lv_coord_t first_key_y = value_box_y + value_box_height + 6;
    lv_obj_t* keyboard = lv_obj_create(settings_menu_overlay_);
    lv_obj_set_size(keyboard, 6 * kKeyWidth + 5 * kKeyGap, 6 * kKeyHeight + 5 * kKeyGap);
    lv_obj_set_pos(keyboard, kFirstX, first_key_y);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_pad_all(keyboard, 0, 0);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        keyboard,
        [](lv_event_t* event) {
            constexpr std::size_t kDrawColumns = 6;
            constexpr lv_coord_t kDrawKeyWidth = 34;
            constexpr lv_coord_t kDrawKeyHeight = 22;
            constexpr lv_coord_t kDrawKeyGap = 4;
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
            lv_obj_t* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
            lv_layer_t* layer = lv_event_get_layer(event);
            lv_area_t object_area;
            lv_obj_get_coords(object, &object_area);

            for (std::size_t index = 0; index < self->text_input_keys_.size(); ++index) {
                const std::size_t row = index / kDrawColumns;
                const std::size_t column = index % kDrawColumns;
                const lv_coord_t x = object_area.x1 + static_cast<lv_coord_t>(column) *
                                                          (kDrawKeyWidth + kDrawKeyGap);
                const lv_coord_t y =
                    object_area.y1 + static_cast<lv_coord_t>(row) * (kDrawKeyHeight + kDrawKeyGap);
                const lv_area_t key_area = {x, y, x + kDrawKeyWidth - 1, y + kDrawKeyHeight - 1};
                const std::string& key = self->text_input_keys_[index];
                const bool active = index == self->text_input_selected_;
                const bool go_key = key == "GO";
                const bool function_key =
                    go_key || key == "ABC" || key == "abc" || key == "123" || key == "<";

                lv_draw_rect_dsc_t rectangle;
                lv_draw_rect_dsc_init(&rectangle);
                rectangle.base.layer = layer;
                rectangle.bg_opa = LV_OPA_COVER;
                rectangle.bg_color =
                    lv_color_hex(active ? (go_key ? kCyberGreen : kCyberYellow)
                                        : (function_key ? kCyberTopBackground : 0x0F1522));
                rectangle.border_width = 1;
                rectangle.border_color = lv_color_hex(active ? kCyberText : 0x1C283D);
                rectangle.radius = 2;
                lv_draw_rect(layer, &rectangle, &key_area);

                lv_draw_label_dsc_t label;
                lv_draw_label_dsc_init(&label);
                label.base.layer = layer;
                label.text = key.c_str();
                label.font = &font_noto_sans_basic_14_1;
                label.color = lv_color_hex(
                    active ? 0x000000
                           : (go_key ? kCyberGreen : (function_key ? kCyberCyan : 0xA3B8CD)));
                label.align = LV_TEXT_ALIGN_CENTER;
                lv_draw_label(layer, &label, &key_area);
            }
        },
        LV_EVENT_DRAW_MAIN_END, this);

    char mode[24];
    snprintf(mode, sizeof(mode), "LEN: %u",
             value != nullptr ? static_cast<unsigned>(strlen(value)) : 0);
    SetCyberHudLocked(password_input ? "KEY" : "SVR", password_input ? "密码输入" : "服务地址",
                      password_input ? "PASSWORD EDIT" : "SERVICE URL", mode, kCyberCyan);
    const bool status_is_error = status_text != nullptr && status_text[0] != '\0' &&
                                 strstr(status_text, "上/下移动") == nullptr;
    if (status_is_error) {
        lv_obj_set_style_text_font(telemetry_label_, text_font, 0);
        lv_obj_set_style_text_color(telemetry_label_, lv_color_hex(kCyberYellow), 0);
        SetCyberTelemetryLocked(status_text);
    } else {
        lv_obj_set_style_text_font(telemetry_label_, &font_noto_sans_basic_14_1, 0);
        lv_obj_set_style_text_color(telemetry_label_, lv_color_hex(0x3F556E), 0);
        SetCyberTelemetryLocked("HOLD U/D = ROW");
    }
    SetCyberFooterLocked("U/D", "移动", "OK", "选择/返回", kCyberYellow);
}

void LcdDisplay::SetSearchResultImage(std::unique_ptr<LvglImage> image, bool full_screen) {
    DisplayLockGuard lock(this);
    SetPreviewImageLocked(std::move(image), full_screen, true);
}

bool LcdDisplay::SetSearchImagePanelRotationLocked(bool rotate_90) {
    if (panel_ == nullptr || panel_io_ == nullptr) {
        return false;
    }
    if (search_image_panel_rotated_ == rotate_90) {
        return true;
    }

    // Complete any queued LVGL/direct-panel transfer before changing MADCTL.
    // ST7789 uses the axis-swap bit plus MY for a clockwise 90-degree view.
    if (esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to drain LCD transfers before image rotation");
        return false;
    }

    const bool target_mirror_x = panel_mirror_x_;
    const bool target_mirror_y = rotate_90 ? !panel_mirror_y_ : panel_mirror_y_;
    const bool target_swap_xy = rotate_90 ? !panel_swap_xy_ : panel_swap_xy_;
    const esp_err_t mirror_result = esp_lcd_panel_mirror(panel_, target_mirror_x, target_mirror_y);
    const esp_err_t swap_result =
        mirror_result == ESP_OK ? esp_lcd_panel_swap_xy(panel_, target_swap_xy) : mirror_result;
    if (swap_result != ESP_OK) {
        ESP_LOGW(TAG, "Search image rotation is unavailable: %s", esp_err_to_name(swap_result));
        // Restore the orientation that was active when this operation began.
        esp_lcd_panel_mirror(panel_, panel_mirror_x_,
                             search_image_panel_rotated_ ? !panel_mirror_y_ : panel_mirror_y_);
        esp_lcd_panel_swap_xy(panel_,
                              search_image_panel_rotated_ ? !panel_swap_xy_ : panel_swap_xy_);
        return false;
    }

    search_image_panel_rotated_ = rotate_90;
    // LVGL knows nothing about the MADCTL change. While the panel is rotated,
    // any LVGL flush would land transposed on the panel (seen on device as
    // jagged bands across the streamed image), so pause invalidation for the
    // whole rotated state and let the direct-panel writes own the screen.
    if (display_ != nullptr) {
        lv_display_enable_invalidation(display_, !rotate_90);
    }
    return true;
}

void LcdDisplay::SetSearchImageTransform(bool flip_x, bool flip_y) {
    search_image_flip_x_ = flip_x;
    search_image_flip_y_ = flip_y;
}

bool LcdDisplay::BeginSearchResultImageStreamLocked(size_t image_width, size_t image_height) {
    if (panel_ == nullptr || panel_io_ == nullptr || display_ == nullptr || image_width == 0 ||
        image_height == 0 || width_ <= 0 || height_ <= 0 ||
        static_cast<size_t>(std::max(width_, height_)) > kSearchImageMaxLinePixels) {
        return false;
    }
    // A streamed source block is decoded row-major. Rotating the panel itself
    // lets each source row be sent as one logical row, so no full-frame
    // transpose buffer is needed. In the rotated panel coordinate space the
    // screen dimensions are exchanged (320x240 for the portrait C3 panel).
    constexpr bool kRotateFullScreenImage = true;
    const uint64_t screen_width =
        kRotateFullScreenImage ? static_cast<uint64_t>(height_) : static_cast<uint64_t>(width_);
    const uint64_t screen_height =
        kRotateFullScreenImage ? static_cast<uint64_t>(width_) : static_cast<uint64_t>(height_);
    const uint64_t source_width = static_cast<uint64_t>(image_width);
    const uint64_t source_height = static_cast<uint64_t>(image_height);

    size_t destination_width = static_cast<size_t>(screen_width);
    size_t destination_height = static_cast<size_t>((source_height * screen_width) / source_width);
    if (destination_height == 0 || destination_height > static_cast<size_t>(screen_height)) {
        destination_height = static_cast<size_t>(screen_height);
        destination_width = static_cast<size_t>((source_width * screen_height) / source_height);
    }
    if (destination_width == 0 || destination_height == 0 || destination_width > screen_width ||
        destination_height > screen_height || destination_width > kSearchImageMaxLinePixels) {
        return false;
    }

    search_image_source_width_ = image_width;
    search_image_source_height_ = image_height;
    search_image_dest_width_ = destination_width;
    search_image_dest_height_ = destination_height;
    search_image_dest_x_ = (static_cast<size_t>(screen_width) - destination_width) / 2;
    search_image_dest_y_ = (static_cast<size_t>(screen_height) - destination_height) / 2;

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    preview_image_cached_.reset();
    ApplyFullScreenSearchLayoutLocked();
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }

    // Refresh the hidden LVGL page once so old text and the letterbox area are
    // cleared. The first direct panel write also drains any queued LVGL SPI
    // transfer before the decoder buffer is reused.
    lv_refr_now(display_);

    if (kRotateFullScreenImage && !SetSearchImagePanelRotationLocked(true)) {
        search_image_stream_active_ = false;
        RestoreSearchPageLayoutLocked();
        lv_refr_now(display_);
        return false;
    }

    // The LVGL refresh above runs through the pre-rotation panel mapping and
    // cannot be relied on to erase a frame the previous stream wrote through
    // the rotated mapping. Fill the whole rotated screen with the background
    // color first, so a switching stream never blends into leftovers of the
    // previous image while its rows sweep across the panel.
    if (!FillScreenWithThemeBackgroundLocked()) {
        search_image_stream_active_ = false;
        SetSearchImagePanelRotationLocked(false);
        RestoreSearchPageLayoutLocked();
        lv_refr_now(display_);
        return false;
    }
    search_image_stream_active_ = true;
    return true;
}

bool LcdDisplay::FillScreenWithThemeBackgroundLocked() {
    if (panel_ == nullptr || panel_io_ == nullptr || width_ <= 0 || height_ <= 0) {
        return false;
    }
    const int fill_width = search_image_panel_rotated_ ? height_ : width_;
    const int fill_height = search_image_panel_rotated_ ? width_ : height_;
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    // The rotated 320x240 result stage uses pure-black 30 px letterboxes for
    // a 16:9 frame, exactly like the prototype. Normal portrait restoration
    // still uses the cyber asphalt background.
    const uint16_t background =
        search_image_panel_rotated_
            ? 0x0000
            : (theme != nullptr ? lv_color_to_u16(theme->background_color()) : 0x0000);
    for (int column = 0; column < fill_width; ++column) {
        search_image_line_buffer_[column * 2] = static_cast<uint8_t>(background >> 8);
        search_image_line_buffer_[column * 2 + 1] = static_cast<uint8_t>(background & 0xFF);
    }
    for (int row = 0; row < fill_height; ++row) {
        if (esp_lcd_panel_draw_bitmap(panel_, 0, row, fill_width, row + 1,
                                      search_image_line_buffer_.data()) != ESP_OK) {
            return false;
        }
    }
    return esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0) == ESP_OK;
}

bool LcdDisplay::DrawSearchResultImageBlock(const Rgb565ImageBlock& block, uint32_t stream_epoch) {
    DisplayLockGuard lock(this);

    if (stream_epoch != search_image_stream_epoch_) {
        // A block from a download that raced with a newer transition (the old
        // worker only checks the cancel flag between blocks). Painting it
        // would restart the stream with the previous image's geometry and
        // leave parts of that image on the panel; reject it so the old worker
        // aborts its decode and exits.
        ESP_LOGI(TAG, "Dropped stale streamed image block (epoch %u != %u)", (unsigned)stream_epoch,
                 (unsigned)search_image_stream_epoch_);
        return false;
    }
    if (!search_image_stream_active_ &&
        !BeginSearchResultImageStreamLocked(block.image_width, block.image_height)) {
        ESP_LOGE(TAG, "Failed to begin image stream: %ux%u", (unsigned)block.image_width,
                 (unsigned)block.image_height);
        return false;
    }
    if (block.data == nullptr || block.width == 0 || block.height == 0 || block.stride == 0 ||
        block.x != 0 || block.width != search_image_source_width_ ||
        block.y > search_image_source_height_ ||
        block.height > search_image_source_height_ - block.y ||
        block.stride < block.width * sizeof(uint16_t) ||
        block.data_size < (block.height - 1) * block.stride + block.width * sizeof(uint16_t)) {
        ESP_LOGE(TAG,
                 "Invalid display block: image=%ux%u block=(%u,%u %ux%u) "
                 "stride=%u data=%u",
                 (unsigned)block.image_width, (unsigned)block.image_height, (unsigned)block.x,
                 (unsigned)block.y, (unsigned)block.width, (unsigned)block.height,
                 (unsigned)block.stride, (unsigned)block.data_size);
        ESP_LOGE(TAG, "Invalid streamed image block");
        return false;
    }

    // The search client requests RGB565_BE, which can be sent directly to the
    // ST7789. Resample each destination row into a small DMA-capable line;
    // this also centralizes horizontal/vertical mirroring and letterboxing.
    for (size_t destination_row = 0; destination_row < search_image_dest_height_;
         ++destination_row) {
        const size_t source_row_in_image =
            (destination_row * search_image_source_height_) / search_image_dest_height_;
        const size_t source_row = search_image_flip_y_
                                      ? search_image_source_height_ - 1 - source_row_in_image
                                      : source_row_in_image;
        if (source_row < block.y || source_row >= block.y + block.height) {
            continue;
        }

        const uint8_t* source_line = block.data + (source_row - block.y) * block.stride;
        for (size_t destination_column = 0; destination_column < search_image_dest_width_;
             ++destination_column) {
            const size_t source_column_in_image =
                (destination_column * search_image_source_width_) / search_image_dest_width_;
            const size_t source_column =
                search_image_flip_x_ ? search_image_source_width_ - 1 - source_column_in_image
                                     : source_column_in_image;
            const size_t block_column = source_column - block.x;
            search_image_line_buffer_[destination_column * 2] = source_line[block_column * 2];
            search_image_line_buffer_[destination_column * 2 + 1] =
                source_line[block_column * 2 + 1];
        }

        const int y = static_cast<int>(search_image_dest_y_ + destination_row);
        const esp_err_t result = esp_lcd_panel_draw_bitmap(
            panel_, static_cast<int>(search_image_dest_x_), y,
            static_cast<int>(search_image_dest_x_ + search_image_dest_width_), y + 1,
            search_image_line_buffer_.data());
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Panel row failed: y=%d err=%s", y, esp_err_to_name(result));
            ESP_LOGE(TAG, "Failed to draw streamed image row: %s", esp_err_to_name(result));
            return false;
        }
    }

    // esp_lcd_panel_draw_bitmap queues SPI color transfers. Drain the queue
    // before jpeg_new_jpeg is allowed to overwrite its reusable block buffer.
    return esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0) == ESP_OK;
}

void LcdDisplay::EndSearchResultImageStream(bool success) {
    DisplayLockGuard lock(this);
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0);
    }
    if (!success) {
        search_image_stream_active_ = false;
        const bool was_rotated = search_image_panel_rotated_;
        SetSearchImagePanelRotationLocked(false);
        if (was_rotated) {
            FillScreenWithThemeBackgroundLocked();
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        RestoreSearchPageLayoutLocked();
        if (was_rotated) {
            lv_obj_invalidate(lv_scr_act());
        }
    }
}

uint32_t LcdDisplay::BeginSearchImageTransition() {
    DisplayLockGuard lock(this);
    const uint32_t epoch = ++search_image_stream_epoch_;
    if (!search_image_stream_active_ && !search_image_panel_rotated_) {
        // No full-screen image is on the panel (e.g. the first result after
        // the search page): keep the normal page visible while loading.
        SetPreviewImageLocked(nullptr, false, true);
        return epoch;
    }

    // Switching between results: keep the full-screen layout and the rotated
    // panel so the next stream paints over the current frame instead of
    // flashing a restored home-page frame in between. The next image block
    // re-begins the stream with fresh geometry via
    // BeginSearchResultImageStreamLocked. Do not invalidate anything here:
    // invalidation is paused while the panel is rotated, and the whole-panel
    // background fill in BeginSearchResultImageStreamLocked erases the old
    // letterbox when the new geometry differs.
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    search_image_stream_active_ = false;
    preview_image_cached_.reset();
    return epoch;
}

void LcdDisplay::SetEmotion(const char* emotion) {
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE || CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3
    // The standalone search application owns the complete body and does not
    // allocate the legacy XiaoZhi emotion layer.
    (void)emotion;
    return;
#endif
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE || CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3
    // Do not resurrect the old XiaoZhi emotion/conversation page when a
    // protocol callback arrives. The search page owns the entire body.
    (void)emotion;
    DisplayLockGuard lock(this);
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    return;
#else

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            if (emoji_box_ != nullptr) {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            if (emoji_box_ != nullptr) {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE && !CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    // The app name is Chinese ("语义搜索") and the built-in basic font has no
    // glyphs for 语/搜/索, so it must share the full theme text font.
    const lv_font_t* header_icon_font = &font_material_symbols_16_4;

    if (app_name_label_ != nullptr) {
        lv_obj_set_style_text_font(app_name_label_, text_font, 0);
    }
    if (search_query_label_ != nullptr) {
        lv_obj_set_style_text_font(search_query_label_, text_font, 0);
    }
    if (search_detail_label_ != nullptr) {
        lv_obj_set_style_text_font(search_detail_label_, text_font, 0);
    }
    if (idle_prompt_main_label_ != nullptr) {
        lv_obj_set_style_text_font(idle_prompt_main_label_, text_font, 0);
    }
    if (idle_prompt_desc_label_ != nullptr) {
        lv_obj_set_style_text_font(idle_prompt_desc_label_, text_font, 0);
    }
    if (footer_left_text_label_ != nullptr) {
        lv_obj_set_style_text_font(footer_left_text_label_, text_font, 0);
    }
    if (footer_right_text_label_ != nullptr) {
        lv_obj_set_style_text_font(footer_right_text_label_, text_font, 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_font(mute_label_, header_icon_font, 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_font(battery_label_, header_icon_font, 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_font(network_label_, header_icon_font, 0);
    }
    if (time_label_ != nullptr) {
        lv_obj_set_style_text_font(time_label_, &font_noto_sans_basic_16_4, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (container_ != nullptr) {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lv_color_hex(kCyberBackground), 0);
    }

    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(top_bar_, lv_color_hex(kCyberTopBackground), 0);
    }
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(status_bar_, lv_color_hex(kCyberSubBackground), 0);
    }

    // Update status bar elements
    if (app_name_label_ != nullptr) {
        lv_obj_set_style_text_color(app_name_label_, lv_color_hex(kCyberText), 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, lv_color_hex(kCyberCyan), 0);
    }
    if (time_label_ != nullptr) {
        lv_obj_set_style_text_color(time_label_, lv_color_hex(kCyberText), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_color(notification_label_, lv_color_hex(kCyberYellow), 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_color(mute_label_, lv_color_hex(kCyberMuted), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, lv_color_hex(kCyberGreen), 0);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lv_color_hex(kCyberText), 0);
    }
    if (subbar_left_label_ != nullptr) {
        lv_obj_set_style_text_color(subbar_left_label_, lv_color_hex(kCyberMuted), 0);
    }
    if (search_query_label_ != nullptr) {
        lv_obj_set_style_text_color(search_query_label_, lv_color_hex(kCyberText), 0);
    }
    if (search_detail_label_ != nullptr) {
        lv_obj_set_style_text_color(search_detail_label_, lv_color_hex(0x6A839E), 0);
    }
    SetCyberAccentLocked(cyber_page_ == CyberPage::Listening || cyber_page_ == CyberPage::Error
                             ? kCyberRed
                             : (cyber_page_ == CyberPage::Connecting ? kCyberYellow : kCyberCyan));

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE && !CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
