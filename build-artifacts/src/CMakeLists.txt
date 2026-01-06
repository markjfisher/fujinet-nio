# __GENERATED_COMMENT__

# This is only used by esp-idf, and not the posix build.

idf_component_register(
    SRCS
# __TARGET_SOURCES_START__
# __TARGET_SOURCES_END__
        ../third_party/cjson/cJSON.c
    INCLUDE_DIRS
        "../include"
        "../third_party/cjson"
    REQUIRES
        yaml-cpp-idf
        esp_psram
        esp_wifi
        esp_netif
        nvs_flash
)
