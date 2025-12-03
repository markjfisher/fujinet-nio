# File: src/CMakeLists.txt

# THIS IS ONLY USED BY ESP-IDF

idf_component_register(
    SRCS
        app/main_esp32.cpp
        lib/bootstrap.cpp
        lib/build_profile.cpp
        lib/fuji_bus_packet.cpp
        lib/fujibus_transport.cpp
        lib/fuji_config_yaml_store.cpp
        lib/fuji_device.cpp
        lib/fujinet_core.cpp
        lib/fujinet_init.cpp
        lib/io_device_manager.cpp
        lib/io_service.cpp
        lib/routing_manager.cpp
        platform/esp32/channel_factory.cpp
        platform/esp32/fuji_config_store_factory.cpp
        platform/esp32/fuji_device_factory.cpp
        platform/esp32/logging.cpp
        platform/esp32/usb_cdc_channel.cpp
        platform/esp32/usb_cdc_channel_factory.cpp

    INCLUDE_DIRS
        "../include"
)