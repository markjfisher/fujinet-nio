#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

/// LED identifiers matching the hardware LEDs
enum class LedId {
    Wifi = 0,  ///< WiFi status LED
    Bus = 1,   ///< Bus activity LED
};

/// Manages status LEDs on the FujiNet device.
///
/// This class provides simple control over the WiFi and Bus LEDs.
/// LEDs are active LOW (set HIGH to turn off, LOW to turn on).
///
/// Usage:
/// @code
/// LedManager leds;
/// leds.set(LedId::Wifi, true);  // Turn WiFi LED on
/// leds.toggle(LedId::Wifi);     // Toggle WiFi LED
/// @endcode
class LedManager {
public:
    LedManager();
    ~LedManager() = default;

    /// Initialize LED GPIO pins.
    /// Must be called once before using other methods.
    void setup();

    /// Set LED state.
    /// @param id Which LED to control
    /// @param on true to turn on, false to turn off
    void set(LedId id, bool on);

    /// Toggle LED state.
    /// @param id Which LED to toggle
    void toggle(LedId id);

    /// Blink an LED a number of times.
    /// This is a blocking call.
    /// @param id Which LED to blink
    /// @param count Number of blinks (default 1)
    void blink(LedId id, int count = 1);

    /// Check if an LED is currently on.
    /// @param id Which LED to check
    /// @return true if the LED is on
    bool isOn(LedId id) const;

private:
    bool _state[2] = {false, false};  ///< Current LED states
    int _pin[2] = {-1, -1};           ///< GPIO pins for each LED
    bool _initialized = false;        ///< True if setup() was called
};

} // namespace fujinet::platform::esp32
