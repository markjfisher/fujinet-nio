# Architecture of fujinet-nio

## Folder structure

```text
fujinet-nio/
├── CMakeLists.txt
├── platformio.ini
├── include/
│   └── fujinet/
│       └── io/
│           └── core/
│               └── io_message.h      # first "core" header, header-only for now
├── src/
│   ├── app/
│   │   └── main_posix.cpp            # entrypoint for POSIX builds
│   └── lib/
│       └── fujinet_init.cpp          # tiny stub to prove library linkage
└── tests/
    ├── CMakeLists.txt
    └── test_smoke.cpp                # first unit test
```


## Build system

The build system is based on CMake, with a PlatformIO wrapper for convenience.

## Platform support

The library is designed to be portable, but the first target is the ESP32.

## Dependencies

The library depends on the following libraries:

- [ESP-IDF](https://github.com/espressif/esp-idf) (for ESP32 support)
- [Catch2](https://github.com/catchorg/Catch2) (for unit testing)

## License

The library is licensed under the MIT license.