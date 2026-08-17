set(SOURCES "esp_xiaozhi_chat_app.c" "esp_xiaozhi_chat_display.c" "app_main.c" "wifi_provisioning.c" "app_audio_dsp.c"
            "app_chat_history.c" "app_sfx.c" "app_camera.c" "http_preview.c"             "app_music_player.c"
            "app_sdcard.c"
            "app_recorder.c"
            "app_camera_preview.c" "app_vision_lite.c" "app_guard_mode.c"
            "app_activity.c"
            "muyu_gif.c" "heart_red.c" "qixi_banner.c"
            "ui_touch.c" "ui_page_manager.c" "ui_control_center.c" "easter_egg.c"
            "page_face.c" "page_launcher.c" "page_dialogue.c" "page_camera.c" "page_music.c" "page_game.c" "page_timer.c")
set(INCLUDE_DIRS "." "../components/esp_xiaozhi/src/priv_include")

idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS ${INCLUDE_DIRS}
                       REQUIRES
                         protocol_examples_common
                         esp_board_manager_adapter
                         esp_xiaozhi
                         jason-mao__av_processor
                         espressif__esp_codec_dev
                         espressif__esp_audio_effects
                         espressif__esp_board_manager
                         espressif__esp_lcd_touch
                         espressif__esp_video
                         espressif__esp_lvgl_port
                         lvgl__lvgl
                         78__xiaozhi-fonts
                         espressif2022__esp_emote_expression
                         espressif2022__esp_emote_gfx
                         espressif__esp_mmap_assets
                         espressif__esp_audio_simple_player
                         espressif__esp_new_jpeg
                         espressif__esp-sr
                         espressif__gmf_ai_audio
                         espressif__mcp-c-sdk
                         esp_http_server
                         esp_http_client
                         espressif__cjson
                         mbedtls
                         lwip
                         nvs_flash
                         esp_wifi
                         esp_event
                         esp_netif
                         fatfs
                         driver
                         esp_timer
                         esp_partition
                       WHOLE_ARCHIVE
                      )
