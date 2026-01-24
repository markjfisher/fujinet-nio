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

# Feature gate for legacy transport support:
# Only enable legacy code paths when building a legacy bus profile (SIO/NetSIO).
#
# ESP-IDF build profiles already define FN_BUILD_ATARI_SIO / FN_BUILD_ATARI_NETSIO at compile time.
target_compile_definitions(${COMPONENT_LIB}
    PUBLIC
        $<$<OR:$<BOOL:${FN_BUILD_ATARI_SIO}>,$<BOOL:${FN_BUILD_ATARI_NETSIO}>>:FN_ENABLE_LEGACY_TRANSPORT>
)
