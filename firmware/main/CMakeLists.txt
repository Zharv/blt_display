idf_component_register(
    SRCS
        "main.c"
        "ui_online.c"
        "ui_wifi.c"
        "ui_printer.c"
        "ui_bms.c"
        "bms_ble.c"
        "netw.c"
        "cp_update.c"
        "print_job.c"
        "fonts.c"
        "i18n.c"
        "hist.c"
        "ui_frame.c"
        "ui_common.c"
        "snap.c"
        "lv_mem_psram.c"
    INCLUDE_DIRS "."
    EMBED_FILES "fonts/Montserrat-Medium.ttf"
    REQUIRES
        esp32_p4_function_ev_board
        report
        print_ipp
        bms
        bt
        esp_wifi_remote
        esp_hosted
        mdns
        esp_http_client
        nvs_flash
        esp_partition
        esp_driver_uart
        esp_netif
        esp_event
)

# LVGL calls lv_malloc_core() & co. from its own library; make sure our
# PSRAM implementation (lv_mem_psram.c) is linked in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=lv_malloc_core")
