idf_component_register(
    SRCS "esp_board_manager_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_board_manager gen_bmgr_codes esp_lvgl_port lvgl
)
