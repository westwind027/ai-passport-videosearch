#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
FONT_SOURCE = REPO_ROOT / "managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_14_1.c"


class SearchRegressionTests(unittest.TestCase):
    def test_search_image_workspace_never_reallocates_large_buffers_at_runtime(self):
        """Pin the device failure: decode storage must survive every HTTP operation."""
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")

        self.assertRegex(
            header,
            r"alignas\(16\)\s+std::array<\s*uint8_t,\s*"
            r"search_memory_budget::kNetworkReceiveBufferBytes>",
        )
        self.assertRegex(
            header,
            r"alignas\(16\)\s+std::array<uint8_t,\s*"
            r"search_memory_budget::kJpegDecodeBufferBytes>",
        )
        self.assertNotIn("heap_caps_aligned_alloc", client)
        self.assertNotIn("ReleaseImageDecodeBuffer", client)
        self.assertNotIn("ReleaseImageReceiveBuffer", client)
        self.assertNotIn("kDecodeBufferAllocAttempts", client)

    def test_search_json_reuses_the_fixed_network_buffer(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        payload_header = (REPO_ROOT / "main/search/search_payload.h").read_text(encoding="utf-8")

        self.assertNotIn("std::string body;", client)
        self.assertIn('endpoint, HTTP_METHOD_GET, "application/json"', client)
        self.assertIn("response_buffer, response_capacity", client)
        self.assertIn("std::string_view body", client)
        self.assertIn("ParseSearchResponse(std::string_view payload", payload_header)

    def test_search_http_is_synchronous_and_has_no_ml307_receive_task(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        cmake = (REPO_ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("#include <esp_http_client.h>", client)
        self.assertIn("esp_http_client_perform", client)
        self.assertIn("HTTP_EVENT_ON_DATA", client)
        self.assertIn("buffer_size = 1024", client)
        self.assertNotIn("CreateHttp", client)
        self.assertNotIn("RetireHttpClient", client)
        helper = client[client.index("BoundedHttpResult PerformBoundedHttpRequest"):
                        client.index("enum class SearchAttemptStatus")]
        self.assertIn("esp_http_client_cleanup(client)", helper)
        self.assertIn("esp_http_client", cmake)

    def test_search_payload_parser_does_not_build_cjson_nodes(self):
        payload = (REPO_ROOT / "main/search/search_payload.cc").read_text(encoding="utf-8")

        self.assertNotIn("#include <cJSON.h>", payload)
        self.assertNotIn("cJSON_ParseWithLength", payload)
        self.assertIn("SearchResult ParseResultItem(std::string_view item", payload)

    def test_search_operations_share_one_static_worker(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")

        self.assertIn("xTaskCreateStatic(TaskEntry, \"video_search\"", client)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", client)
        self.assertIn("xTaskNotifyGive(task_)", client)
        self.assertNotIn("xTaskCreate(TaskEntry", client)
        self.assertIn("StaticTask_t task_storage_", header)
        self.assertIn("std::array<StackType_t, kWorkerStackBytes>", header)

    def test_search_results_are_fixed_capacity_and_client_owned(self):
        payload_header = (REPO_ROOT / "main/search/search_payload.h").read_text(encoding="utf-8")
        client_header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")
        session_header = (REPO_ROOT / "main/search/search_session.h").read_text(encoding="utf-8")

        self.assertIn("class BoundedSearchString", payload_header)
        self.assertIn("std::array<SearchResult, kMaxSearchResults>", payload_header)
        self.assertNotIn("std::vector<SearchResult>", payload_header)
        self.assertIn("SearchResponse search_response_", client_header)
        self.assertIn("const SearchResponse* response_ = nullptr", session_header)
        self.assertIn("sizeof(SearchResponse) <= 6 * 1024", payload_header)

    def test_home_prompt_has_room_for_two_actual_font_lines(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        font = FONT_SOURCE.read_text(encoding="utf-8")
        line_height = int(re.search(r"\.line_height = (\d+)", font).group(1))
        self.assertEqual(line_height, 16)
        cmake = (REPO_ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)", cmake)
        self.assertIn("constexpr lv_coord_t kCyberTopHeight = 28;", source)
        self.assertIn("constexpr lv_coord_t kCyberSubHeight = 18;", source)
        self.assertIn("constexpr lv_coord_t kCyberFooterHeight = 28;", source)
        self.assertIn("lv_obj_set_size(idle_prompt_card_, 216, 62);", source)
        self.assertIn("lv_obj_set_size(search_query_label_, 196, text_font->line_height * 2);", source)
        self.assertIn("lv_obj_set_size(search_detail_label_, 196, text_font->line_height * 2);", source)
        self.assertIn("lv_obj_set_style_text_font(search_detail_label_, text_font, 0);", source)
        self.assertIn("ShowCyberHomeLocked();", source)

    def test_image_variants_keep_the_c3_pipeline_bounded(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        client_header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")
        budget = (REPO_ROOT / "main/search/search_memory_budget.h").read_text(encoding="utf-8")
        self.assertIn("kNetworkReceiveBufferBytes = 14 * 1024", budget)
        self.assertIn("constexpr size_t kMaxDecodedImageBytes", client)
        self.assertIn("image_receive_buffer_.data()", client)
        self.assertIn("image_decode_buffer_.data()", client)
        self.assertIn("jpeg_to_image_into", client)
        self.assertIn("image.owns_data = false", client)
        jpeg = (REPO_ROOT / "main/display/lvgl_display/jpg/jpeg_to_image.c").read_text(encoding="utf-8")
        self.assertIn("jpeg_to_image_scaled_into", jpeg)
        self.assertIn("jpeg_to_image_blocks", jpeg)
        self.assertIn("output_buffer_owned", jpeg)
        self.assertIn("SearchImageVariant::Tiny", client_header)
        image_url_header = (REPO_ROOT / "main/search/search_url.h").read_text(encoding="utf-8")
        self.assertIn("enum class SearchImageVariant", image_url_header)
        self.assertIn("    Small,", image_url_header)
        self.assertIn("BuildSearchImageUrl(image_url, image_variant)", client)
        self.assertIn("kImageHeapCaps", client)
        self.assertNotIn("search image needs a smaller thumbnail", client)
        payload = (REPO_ROOT / "main/search/search_payload.cc").read_text(encoding="utf-8")
        self.assertNotIn("cJSON_ParseWithLength", payload)
        self.assertIn("payload.substr(offset, item_end - offset)", payload)

    def test_search_retries_smaller_responses_after_memory_pressure(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        payload = (REPO_ROOT / "main/search/search_payload.cc").read_text(encoding="utf-8")
        url = (REPO_ROOT / "main/search/search_url.cc").read_text(encoding="utf-8")
        self.assertIn("kSearchResultLimits{{12, 6, 3, 1}}", client)
        self.assertIn("RunSearchAttempt", client)
        self.assertIn("search response is too large", client)
        self.assertIn("search response allocation failed", client)
        self.assertIn("ParseSearchResponse(body, service_url, response, error, result_limit)", client)
        self.assertNotIn("cJSON", payload)
        self.assertIn("max_results = std::min(max_results, kMaxSearchResults)", payload)
        self.assertIn("response.results.PushBack(ParseResultItem", payload)
        self.assertIn("BuildSearchRequestUrl(service_url, query, result_limit)", client)
        self.assertIn('"&limit=" + std::to_string(limit)', url)

    def test_block_image_worker_has_room_for_jpeg_decode_call_chain(self):
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")
        self.assertIn("static constexpr uint32_t kWorkerStackBytes = 8192;", header)
        start = client.index("uint32_t VideoSearchClient::LoadImageBlocks")
        end = client.index("void VideoSearchClient::Cancel", start)
        self.assertIn("xTaskNotifyGive(task_)", client[start:end])

    def test_client_pins_pipeline_buffers_and_waits_for_draining_workers(self):
        # The esp-ml307 TCP receive task allocates HTTP body chunks with
        # operator new; an uncaught std::bad_alloc there abort()s the whole
        # device. Both pipeline buffers must be preallocated once while the
        # heap is still unfragmented. Serial-log evidence showed that
        # re-allocating the decode buffer around downloads deterministically
        # fails: the receive buffer splits the largest free block to just
        # below the decode size. So the decode buffer stays pinned through
        # searches and image downloads, and is only surrendered when a
        # response hits the memory budget. A follow-up request must still
        # wait for a draining worker instead of being silently dropped.
        client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        header = (REPO_ROOT / "main/search/video_search_client.h").read_text(encoding="utf-8")
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        lcd_header = (REPO_ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        self.assertIn("CancelAndWaitForWorkerExit", header)
        self.assertIn("void VideoSearchClient::InitializeWorkspace()", client)
        preallocate_start = client.index("void VideoSearchClient::InitializeWorkspace()")
        run_task = client.index("void VideoSearchClient::RunTask()")
        preallocate_body = client[preallocate_start:run_task]
        self.assertIn("image_receive_buffer_.size()", preallocate_body)
        self.assertIn("image_decode_buffer_.size()", preallocate_body)
        blocks_start = client.index("uint32_t VideoSearchClient::LoadImageBlocks(")
        run_task_pos = client.index("void VideoSearchClient::RunTask()", blocks_start)
        run_task_body = client[run_task_pos:]
        ensure_recv = run_task_body.index("if (!HasSynchronousHttpHeadroom())")
        self.assertLess(ensure_recv, run_task_body.index("PerformBoundedHttpRequest("))
        # The image branch must not release the decode buffer around the
        # download anymore; the pinned buffer rides through it.
        image_branch = run_task_body[run_task_body.index(
            "else if (operation == Operation::Image ||"):run_task_body.index(
            "else if (operation == Operation::PlayerControl)")]
        self.assertNotIn("ReleaseImageDecodeBuffer", image_branch)
        # The search pipeline uses IDF's blocking HTTP client on its own
        # static worker. It therefore has no esp-ml307 receive task, dynamic
        # body queue, or passive-close event-group race.
        self.assertNotIn("CreateHttp", client)
        self.assertNotIn("RetireHttpClient", client)
        self.assertIn("esp_http_client_perform", client)
        # Static buffers are never surrendered. The gate admits synchronous
        # HTTP work with the smaller, recoverable allocation budget.
        headroom_helper = client[client.index(
            "bool VideoSearchClient::HasSynchronousHttpHeadroom() const"):client.index(
            "void VideoSearchClient::InitializeWorkspace()")]
        self.assertIn("kHttpLargestBlockHeadroomBytes", headroom_helper)
        self.assertIn("kHttpTotalHeadroomBytes", headroom_helper)
        self.assertNotIn("ReleaseImage", headroom_helper)
        initialize_start = application.index("void Application::Initialize()")
        initialize_end = application.index("void Application::Run()", initialize_start)
        self.assertIn("video_search_client_.InitializeWorkspace();",
                      application[initialize_start:initialize_end])
        self.assertNotIn("EnsureSearchImageLineBuffer", application)
        self.assertIn("search_image_line_buffer_{}", lcd_header)
        self.assertIn("does not spawn a second receive task", client)
        search_branch = client[run_task_pos:]
        search_body = search_branch[search_branch.index(
            "if (operation == Operation::Search)"):search_branch.index(
            "else if (operation == Operation::Image ||")]
        self.assertNotIn("ReleaseImage", search_body)
        self.assertNotIn("decode_released", search_body)
        self.assertIn("HasSynchronousHttpHeadroom()", search_body)
        # Both entry points used by the application wait for a draining worker.
        search_start = client.index("uint32_t VideoSearchClient::Search(")
        self.assertIn("CancelAndWaitForWorkerExit(1000)", client[search_start:blocks_start])
        self.assertIn("CancelAndWaitForWorkerExit(1000)", client[blocks_start:run_task_pos])

    def test_search_start_closes_codec_input_and_trims_opus_stack(self):
        # The idle power timer kept the codec input open for ~25s after the
        # last spoken word, pinning the input chain's memory through the
        # whole search/image phase. Search start must request an immediate
        # input stop, and the opus codec task must not reserve a 24 KiB
        # stack that its encode/decode workload never uses.
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        service = (REPO_ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        service_header = (REPO_ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
        self.assertIn("audio_service_.RequestInputStop();", application)
        self.assertIn("void AudioService::RequestInputStop()", service)
        self.assertIn("void RequestInputStop();", service_header)
        self.assertIn("AS_EVENT_AUDIO_INPUT_STOP_REQUEST", service)
        # The opus codec stack must stay at the original 24 KiB: on-device
        # high-water logging measured a ~19.7 KiB CELT encoder peak (a 20 KiB
        # stack was left with 772 B of margin); the log guards future tuning.
        self.assertIn('"opus_codec", 2048 * 12', service)
        self.assertIn("uxTaskGetStackHighWaterMark", service)

    def test_activation_serializes_prompt_audio_with_tls_and_survives_audio_oom(self):
        # The device repeatedly rebooted at "Activating... 1/10". Symbolizing the
        # abort stack found operator new -> vector<int16_t> in
        # AudioService::OpusCodecTask while the activation TLS handshake was
        # consuming nearly all internal SRAM. Pairing must serialize those
        # peaks, and both task boundaries must turn allocation pressure into
        # a retry/drop instead of an uncaught exception and device reboot.
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        service = (REPO_ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        service_header = (REPO_ROOT / "main/audio/audio_service.h").read_text(
            encoding="utf-8")

        show_start = application.index("void Application::ShowActivationCode")
        show_end = application.index("void Application::Alert", show_start)
        show_activation = application[show_start:show_end]
        self.assertIn("WaitForPlaybackIdle(kActivationAudioDrainTimeoutMs)", show_activation)
        self.assertIn("StopOutputIfIdle()", show_activation)

        activation_start = application.index("// This will block the loop until the activation")
        activation_end = application.index("void Application::InitializeProtocol", activation_start)
        activation_loop = application[activation_start:activation_end]
        self.assertIn("catch (const std::bad_alloc&)", activation_loop)
        self.assertIn("Activation request ran out of memory", activation_loop)

        self.assertIn("bool WaitForPlaybackIdle(uint32_t timeout_ms);", service_header)
        self.assertIn("bool StopOutputIfIdle();", service_header)
        self.assertIn("bool AudioService::WaitForPlaybackIdle(uint32_t timeout_ms)", service)
        self.assertIn("bool AudioService::StopOutputIfIdle()", service)

        codec_start = service.index("void AudioService::OpusCodecTask()")
        codec_end = service.index("void AudioService::SetDecodeSampleRate", codec_start)
        codec_task = service[codec_start:codec_end]
        self.assertIn("catch (const std::bad_alloc&)", codec_task)
        self.assertIn("Dropping playback frame: out of memory", codec_task)

    def test_home_streams_small_full_screen_directly_without_thumbnails(self):
        # The C3 has only ~26 KiB of free internal heap, so the home page no
        # longer runs a second low-memory Tiny-thumbnail pipeline. Every
        # result streams the Small variant straight onto the panel in
        # full-screen mode, and the footer no longer advertises the removed
        # thumbnail/full-screen toggle.
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        lcd_header = (REPO_ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        search_url = (REPO_ROOT / "main/search/search_url.cc").read_text(encoding="utf-8")
        search_client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(encoding="utf-8")
        self.assertIn("SearchImageVariant::Small", application)
        self.assertNotIn("SearchImageVariant::Tiny", application)
        self.assertIn("void Application::ShowSearchResult(size_t index)", application)
        self.assertNotIn("search_result_full_screen_", application)
        self.assertIn("LoadImageBlocks", application)
        self.assertIn("DrawSearchResultImageBlock", application)
        # Switching results must keep the full-screen layout between streams;
        # otherwise a restored home-page frame flashes between two images.
        # The transition returns an epoch token that the block callback must
        # carry so blocks from a superseded download are rejected instead of
        # restarting the stream with the previous image's geometry (which
        # left fragments of that image on the panel).
        self.assertIn("lcd_display->BeginSearchImageTransition()", application)
        self.assertIn("uint32_t image_epoch = 0;", application)
        self.assertIn("image_epoch = lcd_display->BeginSearchImageTransition()", application)
        self.assertIn("DrawSearchResultImageBlock(block, image_epoch)", application)
        self.assertIn("uint32_t LcdDisplay::BeginSearchImageTransition()", display)
        self.assertIn("stream_epoch != search_image_stream_epoch_", display)
        self.assertIn("search_image_stream_epoch_ = 0;", lcd_header)
        # While the panel is rotated for the streamed image, LVGL flushing
        # must be paused: a flush in that state lands transposed on the panel
        # (seen on device as jagged bands during image switching).
        self.assertIn("lv_display_enable_invalidation(display_, !rotate_90)", display)
        self.assertNotIn("lv_obj_invalidate(lv_scr_act());\n}\n\nvoid LcdDisplay::SetEmotion", display)
        # A page flip mid-download must abort the old HTTP body read quickly
        # so the next image load is not rejected by the worker-exit timeout.
        self.assertIn("const std::function<bool()>& should_abort", search_client)
        self.assertIn("[this]() { return IsCancelRequested(); }", search_client)
        self.assertIn("SetPreviewImageLocked(nullptr, false, true)", display)
        self.assertIn("ApplyFullScreenSearchLayoutLocked", display)
        self.assertIn("SetSearchImagePanelRotationLocked(true)", display)
        self.assertIn("const uint64_t screen_width =", display)
        self.assertIn("kRotateFullScreenImage ? static_cast<uint64_t>(height_)", display)
        self.assertIn("lv_obj_set_size(preview_image_, width_, height_)", display)
        # The pre-rotation LVGL refresh cannot reliably erase a frame the
        # previous stream wrote through the rotated panel mapping, so the
        # stream begin and every rotation-restore path must fill the whole
        # panel with the background color before new pixels are shown.
        self.assertIn("lv_color_to_u16(theme->background_color())", display)
        self.assertIn("bool FillScreenWithThemeBackgroundLocked();", lcd_header)
        self.assertEqual(display.count("FillScreenWithThemeBackgroundLocked();"), 2)
        # The full-screen stream keeps the service's native Small variant and
        # fits it by screen aspect without any client-side upscaling.
        self.assertNotIn("&width=320&height=240", search_url)
        self.assertIn("(source_height * screen_width) / source_width", display)
        self.assertIn(
            'SetCyberFooterLocked("U/D", "结果翻页", "HOLD", "全屏", kCyberYellow)',
            display,
        )
        self.assertRegex(display, r"search_image_panel_rotated_\s*\? 0x0000")

    def test_home_header_uses_fixed_slots_for_all_status_items(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        setup_start = source.index("#else\nvoid LcdDisplay::SetupUI()")
        setup_end = source.index("void LcdDisplay::RestoreSearchPageLayoutLocked()", setup_start)
        setup = source[setup_start:setup_end]

        self.assertIn("lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);", setup)
        self.assertNotIn("lv_obj_set_flex_flow(top_bar_,", setup)
        self.assertIn("font_noto_sans_basic_16_4", setup)
        self.assertIn("font_material_symbols_16_4", setup)
        self.assertIn("lv_obj_set_pos(app_name_label_,", setup)
        self.assertIn("lv_obj_set_pos(right_icons,", setup)
        self.assertIn("lv_obj_set_size(time_label_,", setup)
        self.assertIn("lv_obj_set_size(network_label_,", setup)
        self.assertIn("lv_obj_set_size(battery_label_,", setup)

    def test_cyber_ui_does_not_render_decorative_double_slash_labels(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        rendered_literals = re.findall(r'"([^"\n]*//[^"\n]*)"', source)
        self.assertEqual(
            [],
            rendered_literals,
            "UI text must not use slash-slash separators; use a short user-facing label instead",
        )

    def test_cyber_ui_text_slots_are_bounded_and_do_not_cover_each_other(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        self.assertIn("constexpr lv_coord_t kCyberTelemetryHeight = 18;", source)
        self.assertIn("constexpr lv_coord_t row_height = 30;", source)
        self.assertIn("constexpr lv_coord_t row_gap = 3;", source)
        self.assertIn("const lv_coord_t kBottom = kCyberTelemetryTop - 4;", source)
        self.assertIn("lv_label_set_long_mode(telemetry_label_, LV_LABEL_LONG_DOT);", source)
        self.assertIn("lv_obj_set_size(brand_tag_label_, 28, 16);", source)
        self.assertIn("constexpr lv_coord_t kHeaderAppWidth = 80;", source)
        self.assertIn("lv_obj_set_size(key_label, 34, 18);", source)
        self.assertIn("lv_obj_set_pos(text_label, x + 44, 5);", source)

    def test_cyberpunk_home_avoids_heap_heavy_compatibility_widgets(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        setup_start = source.index("#else\nvoid LcdDisplay::SetupUI()")
        setup_end = source.index("\nvoid LcdDisplay::SetCyberAccentLocked", setup_start)
        setup = source[setup_start:setup_end]

        self.assertNotIn("emoji_box_ = lv_obj_create", setup)
        self.assertNotIn("emoji_image_ = lv_img_create", setup)
        self.assertNotIn("bottom_bar_ = lv_obj_create", setup)
        self.assertNotIn("button_hint_label_ = lv_label_create", setup)
        self.assertNotIn("voice_bars_[index] = lv_obj_create", setup)
        self.assertNotIn("hud_corners_[index] = lv_obj_create", setup)

    def test_setup_pages_keep_the_home_chrome_visible(self):
        display = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        # Settings and keyboard share the exact 46..291 body viewport; the
        # top HUD, telemetry strip, and footer remain visible and unchanged.
        self.assertEqual(
            display.count(
                "lv_obj_set_size(settings_menu_overlay_, screen_width, "
                "kCyberFooterTop - kCyberBodyTop);"
            ),
            2,
        )
        self.assertEqual(
            display.count("lv_obj_set_pos(settings_menu_overlay_, 0, kCyberBodyTop);"),
            2,
        )
        self.assertIn("lv_obj_move_foreground(top_bar_);", display)
        self.assertIn("lv_obj_move_foreground(status_bar_);", display)
        self.assertIn("lv_obj_move_foreground(telemetry_label_);", display)
        self.assertIn("lv_obj_move_foreground(button_hint_bar_);", display)
        self.assertIn("RestoreHomeButtonHintLocked();", display)

        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        initialize_start = application.index("void Application::Initialize()")
        initialize_end = application.index("void Application::Run()", initialize_start)
        self.assertIn("display->ResetSearchContent();", application[initialize_start:initialize_end])

    def test_stt_path_normalizes_before_display_and_search(self):
        source = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn('#include "search/search_query.h"', source)
        self.assertRegex(source, r"NormalizeSearchQuery\(message\)")
        self.assertIn("HandleSearchText(query)", source)

    def test_search_stops_xiaozhi_before_allocating_http_client(self):
        source = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = source.index("void Application::HandleSearchText")
        end = source.index("void Application::HandleSearchCompleted", start)
        function = source[start:end]
        self.assertLess(function.index("protocol_->CloseAudioChannel();"),
                        function.index("video_search_client_.Search("))
        self.assertLess(function.index("EnableWakeWordDetection(false)"),
                        function.index("video_search_client_.Search("))
        self.assertLess(function.index("ReleaseWakeWordResources()"),
                        function.index("video_search_client_.Search("))

    def test_chat_clear_does_not_reset_search_page(self):
        source = (REPO_ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        start = source.rindex("void LcdDisplay::ClearChatMessages()")
        end = source.index("\n}\n", start) + 3
        clear_function = source[start:end]
        self.assertNotIn("search_detail_label_", clear_function)
        self.assertNotIn("preview_image_", clear_function)
        self.assertIn("void LcdDisplay::ResetSearchContent()", source)

        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        idle_start = application.index("case kDeviceStateIdle:")
        idle_end = application.index("case kDeviceStateConnecting:", idle_start)
        self.assertIn("display->ResetSearchContent();", application[idle_start:idle_end])
        idle_function = application[idle_start:idle_end]
        self.assertIn("if (search_active)", idle_function)
        self.assertIn("ReleaseWakeWordResources();", idle_function)
        self.assertIn("EnableWakeWordDetection(true);", idle_function)

    def test_long_press_results_send_remote_player_commands(self):
        # In the full-screen result view, long-press UP asks the backend web
        # player to open the current media at the scene timestamp, and
        # long-press DOWN closes the remote player. Both commands must ride
        # the shared VideoSearchClient worker (reusing its task stack and
        # HTTP client lifecycle, serialized with the image pipeline) instead
        # of spawning a detached task that races image transfers for heap.
        application = (REPO_ROOT / "main/application.cc").read_text(encoding="utf-8")
        cmake = (REPO_ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        search_url = (REPO_ROOT / "main/search/search_url.cc").read_text(encoding="utf-8")
        player_client = (REPO_ROOT / "main/search/player_control_client.cc").read_text(
            encoding="utf-8")
        search_client = (REPO_ROOT / "main/search/video_search_client.cc").read_text(
            encoding="utf-8")
        self.assertIn("search/player_control_client.cc", cmake)
        self.assertIn("std::string BuildPlayerControlUrl(const std::string& service_url)",
                      search_url)
        self.assertIn('ResolveSearchUrl(service_url, "/v1/player/control")', search_url)
        # The play payload follows the backend contract and seeks to the
        # scene timestamp of the displayed result.
        for fragment in ("\"media_id\"", "\"time\"", "\"fullscreen\"", "\"autoplay\""):
            self.assertIn(fragment, player_client)
        self.assertIn("\"action\", \"close\"", player_client)
        # The facade only builds the payload; the POST itself runs on the
        # shared worker. No detached task or esp-ml307 receive task.
        self.assertNotIn("xTaskCreate", player_client)
        self.assertNotIn("CreateHttp", player_client)
        self.assertIn("client.SendPlayerControl(url, json", player_client)
        self.assertIn("bool VideoSearchClient::SendPlayerControl", search_client)
        self.assertIn("if (task_ == nullptr || worker_running_) {\n        return false;\n    }",
                      search_client)
        self.assertIn("player_control_url, HTTP_METHOD_POST", search_client)
        self.assertIn("Operation::PlayerControl", search_client)
        # Image workspaces are fixed storage; remote commands cannot cause a
        # decode-buffer release/reallocation cycle.
        self.assertNotIn("kDecodeBufferAllocBytes", search_client)
        self.assertNotIn("ReleaseImageDecodeBuffer", search_client)
        up_start = application.index("void Application::HandleUpButtonEvent(bool fast)")
        down_start = application.index("void Application::HandleDownButtonEvent(bool fast)")
        up_function = application[up_start:down_start]
        down_end = application.index("void Application::HandleSettingsConfirm()", down_start)
        down_function = application[down_start:down_end]
        self.assertIn(
            "PlayerControlClient::Play(video_search_client_, search_service_url_",
            up_function)
        # The play time must mirror the web player's openPlayer selection:
        # the preview frame's own timestamp first, scene start as fallback.
        self.assertIn("result.preview_time >= 0.0 ? result.preview_time : result.start",
                      up_function)
        payload = (REPO_ROOT / "main/search/search_payload.cc").read_text(encoding="utf-8")
        self.assertIn('NumberValue(item, "preview_time", -1.0)', payload)
        self.assertIn("远程播放", up_function)
        self.assertIn("PlayerControlClient::Close(video_search_client_, search_service_url_)",
                      down_function)
        self.assertIn("关闭远程播放", down_function)
        # A dropped command (worker busy) must be surfaced to the user.
        self.assertIn("设备忙", up_function)
        self.assertIn("设备忙", down_function)


if __name__ == "__main__":
    unittest.main()
