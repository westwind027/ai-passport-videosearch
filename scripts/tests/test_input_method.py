import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class LeoRadioInputMethodTests(unittest.TestCase):
    def test_keyboard_footer_explains_navigation_and_selection(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        display = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        self.assertIn(
            'status != nullptr ? status : "上/下移动  长按跨行"',
            application,
        )
        self.assertIn('"OK选择  GO提交  长按返回"', application)
        self.assertIn(
            'SetCyberTelemetryLocked("HOLD U/D = ROW")',
            display,
        )
        self.assertIn(
            'SetCyberFooterLocked("U/D", "移动", "OK", "选择/返回", kCyberYellow)',
            display,
        )

    def test_keyboard_chinese_text_never_uses_the_basic_font(self):
        # The built-in basic fonts have no glyphs for most of the Chinese text
        # on the keyboard page (footer hints, value placeholder) and the app
        # name in the header. Those widgets must bind the full theme font.
        display = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        self.assertNotIn(
            "lv_obj_set_style_text_font(app_name_label_, header_text_font, 0)",
            display,
        )
        self.assertNotIn(
            "const lv_font_t* value_font = &font_noto_sans_basic_16_4;",
            display,
        )
        self.assertIn('lv_label_set_text(app_name_label_, "语义搜索")', display)
        # The value box renders URLs/passwords (pure ASCII) in the smaller
        # built-in Latin font and only falls back to the theme font for the
        # Chinese "（空）" placeholder, so the value always fits one line.
        self.assertIn(
            "const lv_font_t* value_font = value_is_ascii ? &font_noto_sans_basic_14_1 : text_font;",
            display,
        )
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)", cmake)

    def test_service_address_is_edited_without_its_scheme(self):
        # Stripping the scheme keeps the value box on one line; commit
        # re-attaches the remembered scheme before validation and storage.
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")

        self.assertIn('text_input_scheme_ = "http://";', application)
        self.assertIn('for (const char* prefix : {"https://", "http://"})', application)
        self.assertIn("std::string server_url = text_input_scheme_ + text_input_value_;", application)
        self.assertIn('settings.SetString("server_url", server_url);', application)

    def test_keyboard_pages_keep_the_original_key_contract(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        lower = application[application.index("kLowerTextInputKeys"):]
        upper = application[application.index("kUpperTextInputKeys"):]
        symbols = application[application.index("kSymbolTextInputKeys"):]

        self.assertIn('"ABC", "123", "<", "GO"', lower)
        self.assertIn('"abc", "123", "<", "GO"', upper)
        self.assertIn('"abc", "ABC", "<", "GO"', symbols)
        self.assertNotIn('"清空"', application)

    def test_long_navigation_is_a_single_keyboard_row(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        board = (ROOT / "main/boards/folo/ai-passport-c3/folo_ai_passport_c3_board.cc").read_text(
            encoding="utf-8"
        )
        display = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        self.assertIn("MAIN_EVENT_BUTTON_UP_LONG", application)
        self.assertIn("MAIN_EVENT_BUTTON_DOWN_LONG", application)
        self.assertIn("fast ? std::min<std::size_t>(6, keys.size()) : 1", application)
        self.assertIn("new AdcButton(adc_config, 850)", board)
        self.assertIn("HandleUpButtonLong", board)
        self.assertIn("HandleDownButtonLong", board)
        self.assertIn("kKeyWidth = 34", display)
        self.assertIn("kKeyHeight = 22", display)
        self.assertIn("kKeyGap = 4", display)
        self.assertIn("kFirstX = 8", display)
        # The 6x5 matrix starts immediately below the compact value slot and
        # leaves the shared telemetry/footer strips intact.
        self.assertIn("const lv_coord_t first_key_y = value_box_y + value_box_height + 6;", display)
        self.assertIn("kCyberFooterTop - kCyberBodyTop", display)
        self.assertIn(
            'SetCyberFooterLocked("U/D", "移动", "OK", "选择/返回", kCyberYellow)',
            display,
        )
        self.assertIn('"OK选择  GO提交  长按返回"', application)

    def test_keyboard_matrix_is_drawn_without_per_key_lvgl_objects(self):
        display = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        start = display.index("void LcdDisplay::ShowTextInput")
        end = display.index("void LcdDisplay::SetSearchResultImage", start)
        text_input = display[start:end]

        self.assertIn("DrawCyberVoiceSpectrum", display)
        self.assertIn("lv_draw_rect(layer, &rectangle, &key_area)", text_input)
        self.assertIn("lv_draw_label(layer, &label, &key_area)", text_input)
        self.assertNotIn("lv_obj_t* key_box = lv_obj_create", text_input)
        self.assertNotIn("lv_obj_t* key_label = lv_label_create", text_input)


if __name__ == "__main__":
    unittest.main()
