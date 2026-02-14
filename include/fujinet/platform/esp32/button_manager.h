#pragma once

#include <functional>
#include <cstdint>

namespace fujinet::platform::esp32 {

/// Button identifiers matching the hardware buttons
enum class ButtonId {
    A = 0,  ///< Button A (general purpose)
    B = 1,  ///< Button B (general purpose)
    C = 2,  ///< Button C (safe reset)
};

/// Button event types
enum class ButtonEvent {
    None,       ///< No event
    ShortPress, ///< Quick press and release
    LongPress,  ///< Press held for > 1 second
    DoubleTap,  ///< Two quick presses in succession
};

/// Callback type for button events
using ButtonCallback = std::function<void(ButtonId id, ButtonEvent event)>;

/// Manages physical buttons on the FujiNet device.
///
/// This class handles debouncing, long-press detection, and double-tap
/// detection for up to 3 buttons (A, B, C). Button C is typically used
/// for safe reset functionality.
///
/// Buttons are active LOW (pulled high, pressed when reading 0).
class ButtonManager {
public:
    ButtonManager();
    ~ButtonManager();

    /// Set the callback to be invoked on button events.
    /// @param callback Function to call when a button event occurs
    void setCallback(ButtonCallback callback);

    /// Start the button monitoring task.
    /// Does nothing if already running or if no buttons are enabled.
    void start();

    /// Stop the button monitoring task.
    void stop();

    /// Check if a button is currently pressed.
    /// @param button Which button to check
    /// @return true if the button is currently being held down
    bool isPressed(ButtonId button) const;

private:
    // Timing constants (in milliseconds)
    static constexpr unsigned long LONG_PRESS_TIME = 1000;  // 1 second for long press
    static constexpr unsigned long DOUBLE_TAP_TIME = 400;   // 400ms window for double tap
    static constexpr unsigned long IGNORE_TIME = 0xFFFFFFFF; // Sentinel value

    /// Internal state for each button
    struct ButtonState {
        int pin = -1;               ///< GPIO pin number (-1 if disabled)
        bool disabled = true;       ///< True if button is not configured
        bool active = false;        ///< True while button is being held
        unsigned long pressStartMs = 0; ///< Time when current press started
        unsigned long lastTapMs = 0;    ///< Time of last tap (for double-tap detection)
    };

    /// FreeRTOS task entry point
    static void buttonTask(void* param);

    /// Poll all buttons and generate events
    void pollButtons();

    /// Check a single button and return any event
    ButtonEvent checkButton(ButtonId button);

    ButtonState _buttons[3];       ///< State for buttons A, B, C
    ButtonCallback _callback;      ///< User-provided callback
    void* _taskHandle = nullptr;   ///< FreeRTOS task handle
};

} // namespace fujinet::platform::esp32
