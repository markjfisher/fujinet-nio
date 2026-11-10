#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

/// LED identifiers matching the hardware LEDs
enum class LedId {
    Wifi = 0,  ///< WiFi status LED
    Bus = 1,   ///< Bus activity LED
    Bluetooth = 2,
};

// Called only after application network bytes have actually been transferred.
// It never waits for a timer or GPIO operation.
void note_network_activity() noexcept;

// Same pulse for local file/SD transfers.
void note_activity() noexcept;

// Timer-side pulse state; the counter can coalesce any number of events.
class NetworkActivityPulse {
public:
    bool tick(std::uint32_t count) noexcept {
        if (count != _seen) { _seen = count; _ticks = 4; }
        if (!_ticks) return false;
        --_ticks;
        return true;
    }
private:
    std::uint32_t _seen{0};
    unsigned _ticks{0};
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
    ~LedManager();

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
    bool _state[3] = {false, false, false};
    int _pin[3] = {-1, -1, -1};
    bool _initialized = false;        ///< True if setup() was called
    void* _activity_timer = nullptr;
    NetworkActivityPulse _activity_pulse;
    static void activity_tick(void* arg);
};

} // namespace fujinet::platform::esp32
