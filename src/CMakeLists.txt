# File: src/CMakeLists.txt

# THIS IS ONLY USED BY ESP-IDF

idf_component_register(
    SRCS
# __TARGET_SOURCES_START__
        app/main_esp32.cpp
        lib/bootstrap.cpp
        lib/build_profile.cpp
        lib/clock_device.cpp
        lib/clock_device_init.cpp
        lib/file_device.cpp
        lib/file_device_init.cpp
        lib/fs_stdio.cpp
        lib/fuji_bus_packet.cpp
        lib/fuji_config_yaml_store.cpp
        lib/fuji_device.cpp
        lib/fujibus_transport.cpp
        lib/fujinet_core.cpp
        lib/fujinet_init.cpp
        lib/io_device_manager.cpp
        lib/io_service.cpp
        lib/network_device.cpp
        lib/network_device_init.cpp
        lib/network_protocol_registry.cpp
        lib/network_protocol_stub.cpp
        lib/routing_manager.cpp
        lib/storage_manager.cpp
        platform/esp32/channel_factory.cpp
        platform/esp32/fs_factory.cpp
        platform/esp32/fs_init.cpp
        platform/esp32/fuji_config_store_factory.cpp
        platform/esp32/fuji_device_factory.cpp
        platform/esp32/hardware_caps.cpp
        platform/esp32/logging.cpp
        platform/esp32/network_registry.cpp
        platform/esp32/pinmap.cpp
        platform/esp32/time.cpp
        platform/esp32/usb_cdc_channel.cpp
# __TARGET_SOURCES_END__
        ../third_party/cjson/cJSON.c
    INCLUDE_DIRS
        "../include"
        "../third_party/cjson"
    REQUIRES
        yaml-cpp-idf
        esp_psram
)
