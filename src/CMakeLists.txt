# File: src/CMakeLists.txt

# THIS IS ONLY USED BY ESP-IDF

# Collect all core library sources under src/lib
file(GLOB LIB_SOURCES
    "lib/*.cpp"
)

idf_component_register(
    SRCS
        app/main_esp32.cpp
        ${LIB_SOURCES}
    INCLUDE_DIRS
        "../include"
)
