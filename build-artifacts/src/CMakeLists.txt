# __GENERATED_COMMENT__

# This is only used by esp-idf, and not the posix build.

set(FN_IDF_SOURCES
# __TARGET_SOURCES_START__
        lib/json_content_translator.cpp
# __TARGET_SOURCES_END__
        ../third_party/cjson/cJSON.c
        ../third_party/cjson/cJSON_Utils.c
)

# Palm HotSync (docs/hotsync.md): its sources, or only the stub start_hotsync()
# without them. FN_WITH_HOTSYNC is a CMake variable, not a compiler flag: a
# profile that wants HotSync passes `-D FN_WITH_HOTSYNC=ON` in its
# board_build.cmake_extra_args (pio-build/ini/platforms/*.ini).
if(FN_WITH_HOTSYNC)
    list(APPEND FN_IDF_SOURCES
# __HOTSYNC_SOURCES_START__
# __HOTSYNC_SOURCES_END__
    )
else()
    list(APPEND FN_IDF_SOURCES
# __HOTSYNC_OFF_SOURCES_START__
# __HOTSYNC_OFF_SOURCES_END__
    )
endif()

idf_component_register(
    SRCS
        ${FN_IDF_SOURCES}
    INCLUDE_DIRS
        "../include"
        "../third_party/cjson"
        "../third_party/stb"
    REQUIRES
        yaml-cpp-idf
        esp_psram
        esp_wifi
        esp_netif
        nvs_flash
)

#
# NOTE:
# PlatformIO/ESP-IDF builds provide transport selection via compiler flags
# (e.g. `-DFN_BUILD_ATARI_SIO`), so most sources are not feature-gated here;
# code is gated by those compiler flags. HotSync is the exception above: the
# build selects its sources, and they carry no feature guards.
