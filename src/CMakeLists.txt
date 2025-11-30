# File: src/CMakeLists.txt

# THIS IS ONLY USED BY ESP-IDF

idf_component_register(
    SRCS
        app/main_esp32.cpp
        lib/fujinet_init.cpp
        lib/fujinet_core.cpp
        lib/io_device_manager.cpp
        lib/routing_manager.cpp
        lib/io_service.cpp
        lib/rs232_transport.cpp
        lib/build_profile.cpp
        lib/bootstrap.cpp
        platform/esp32/channel_factory.cpp
        platform/esp32/usb_cdc_channel.cpp
    INCLUDE_DIRS
        "../include"
)