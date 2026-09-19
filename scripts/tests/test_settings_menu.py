import pathlib
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


class SettingsMenuRepositoryTests(unittest.TestCase):
    def test_settings_menu_uses_a_dedicated_full_screen_display_layer(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        display_header = (REPO_ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        display_source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        start = application.index("void Application::RenderSettingsMenu()")
        end = application.index("void Application::AdjustVolume", start)
        render_function = application[start:end]

        self.assertIn(
            'display->ShowSettingsMenu(items, settings_index_, nullptr, "OK进入  长按返回");',
            render_function,
        )
        self.assertNotIn('display->SetChatMessage("system", menu.c_str())', render_function)
        self.assertIn("ShowSettingsMenu", display_header)
        self.assertIn("settings_menu_overlay_", display_header)
        self.assertIn("lv_label_set_long_mode(label, LV_LABEL_LONG_DOT)", display_source)
        self.assertIn("settings_volume_mode_", application)
        self.assertIn('"音量设置", "上/下调节  OK确认  长按返回设置"', application)

    def test_long_ok_is_parent_navigation_for_all_menu_pages(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = application.index("void Application::HandleEnterSettingsEvent()")
        end = application.index("void Application::HandleUpButtonEvent", start)
        handler = application[start:end]

        self.assertIn("ui_mode_ == UiMode::WifiList", handler)
        self.assertIn("ui_mode_ == UiMode::TextInput", handler)
        self.assertIn("ui_mode_ = UiMode::Settings;", handler)
        self.assertIn("GO is the explicit save/submit", handler)
        self.assertNotIn("EnterWifiConfigMode", handler)
        self.assertNotIn("CommitTextInput();", handler)

        self.assertNotIn("SearchConfigServer", application)
        self.assertNotIn("search_config_server", application)
        self.assertNotIn(":8080", application)

    def test_settings_rows_are_visible_and_device_info_is_removed(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        render_start = application.index("void Application::RenderSettingsMenu()")
        render_end = application.index("void Application::RenderVolumeMenu()", render_start)
        render_function = application[render_start:render_end]
        self.assertIn("constexpr size_t kSettingsItemCount = 4;", application)
        self.assertNotIn('"设备信息"', render_function)
        self.assertNotIn("SystemInfo::GetUserAgent()", application[render_start:])
        self.assertIn("row_index * (row_height + row_gap)", display)

    def test_initial_search_shell_starts_like_the_home_page(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        initialize_start = application.index("void Application::Initialize()")
        initialize_end = application.index("void Application::Run()", initialize_start)
        initialize = application[initialize_start:initialize_end]
        self.assertRegex(
            initialize,
            r"display->ResetSearchContent\(\);[\s\S]*"
            r"display->SetStatus\(Lang::Strings::STANDBY\);",
        )

        setup_start = display.index("#else\nvoid LcdDisplay::SetupUI()")
        setup_end = display.index("void LcdDisplay::RestoreSearchPageLayoutLocked()", setup_start)
        setup = display[setup_start:setup_end]
        self.assertIn('lv_label_set_text(status_label_, "SYS_STANDBY");', setup)
        self.assertIn("ShowCyberHomeLocked();", display)

    def test_folo_lcd_never_compiles_the_legacy_xiaozhi_screen(self):
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        self.assertIn(
            "#if CONFIG_USE_WECHAT_MESSAGE_STYLE && !CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3",
            display,
        )
        self.assertIn(
            "#if !CONFIG_USE_WECHAT_MESSAGE_STYLE || CONFIG_BOARD_TYPE_FOLO_AI_PASSPORT_C3",
            display,
        )

    def test_startup_does_not_cover_search_home_with_network_notifications(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")

        network_start = application.index("board.SetNetworkEventCallback")
        network_end = application.index("// Start network asynchronously", network_start)
        network_callback = application[network_start:network_end]
        self.assertNotIn("display->ShowNotification", network_callback)
        self.assertIn("display->SetStatus(Lang::Strings::CONNECTING);", network_callback)

        activation_start = application.index("void Application::HandleActivationDoneEvent()")
        activation_end = application.index("void Application::CheckAssetsVersion()", activation_start)
        activation_handler = application[activation_start:activation_end]
        self.assertNotIn("display->ShowNotification", activation_handler)
        self.assertIn("display->SetStatus(Lang::Strings::STANDBY);", activation_handler)

    def test_settings_pages_do_not_repeat_titles_and_keep_service_url_readable(self):
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        settings_start = application.index("void Application::RenderSettingsMenu()")
        settings_end = application.index("void Application::RenderVolumeMenu()", settings_start)
        settings = application[settings_start:settings_end]
        self.assertIn('"服务地址配置"', settings)
        self.assertIn(
            'display->ShowSettingsMenu(items, settings_index_, nullptr, "OK进入  长按返回");',
            settings,
        )
        self.assertNotIn('"搜索服务地址"', settings)

        wifi_start = application.index("void Application::RenderWifiList()")
        wifi_end = application.index("void Application::RenderTextInput", wifi_start)
        wifi = application[wifi_start:wifi_end]
        self.assertIn('display->ShowSettingsMenu(items, selected, nullptr,', wifi)
        self.assertIn('"上/下选择  OK进入  长按返回");', wifi)

        input_start = application.index("void Application::RenderTextInput")
        input_end = application.index("void Application::HandleWifiListConfirm", input_start)
        text_input = application[input_start:input_end]
        self.assertIn('const char* title = wifi_password ? "网络设置" : "服务地址配置";', text_input)
        self.assertIn('const char* context = wifi_password ? wifi_selected_ssid_.c_str() : nullptr;', text_input)
        self.assertNotIn('"搜索服务地址"', text_input)

        self.assertIn('password_input ? "PASSWORD TERMINAL" : "REMOTE REST URL"', display)
        self.assertIn('password_input ? "密码输入" : "服务地址"', display)
        self.assertIn("lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);", display)

    def test_hidden_settings_page_releases_its_lvgl_tree(self):
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        start = display.index("void LcdDisplay::HideSettingsMenu()")
        end = display.index("void LcdDisplay::SetSearchContent", start)
        hide = display[start:end]

        self.assertIn("lv_obj_delete(settings_menu_overlay_)", hide)
        self.assertIn("settings_menu_overlay_ = nullptr", hide)
        self.assertIn("text_input_keys_.shrink_to_fit()", hide)


if __name__ == "__main__":
    unittest.main()
