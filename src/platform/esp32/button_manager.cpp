#include "fujinet/platform/esp32/button_manager.h"
#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/core/logging.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_timer.h"

namespace fujinet::platform::esp32 {

static const char* TAG = "button";

ButtonManager::ButtonManager()
{
    const PinMap& pins = pinmap();

    // Initialize button A
    _buttons[static_cast<int>(ButtonId::A)].pin = pins.button.a;
    _buttons[static_cast<int>(ButtonId::A)].disabled = (pins.button.a < 0);

    // Initialize button B
    _buttons[static_cast<int>(ButtonId::B)].pin = pins.button.b;
    _buttons[static_cast<int>(ButtonId::B)].disabled = (pins.button.b < 0);

    // Initialize button C (safe reset)
    _buttons[static_cast<int>(ButtonId::C)].pin = pins.button.c;
    _buttons[static_cast<int>(ButtonId::C)].disabled = (pins.button.c < 0);

    // Configure GPIO for each enabled button
    for (int i = 0; i < 3; ++i) {
        if (!_buttons[i].disabled && _buttons[i].pin >= 0) {
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pin_bit_mask = (1ULL << _buttons[i].pin);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Buttons are active LOW
            esp_err_t err = gpio_config(&io_conf);
            if (err != ESP_OK) {
                FN_LOGE(TAG, "Failed to configure button %d GPIO %d: %d", i, _buttons[i].pin, err);
                _buttons[i].disabled = true;
            } else {
                FN_LOGI(TAG, "Button %c enabled on GPIO %d", 'A' + i, _buttons[i].pin);
            }
        }
    }
}

ButtonManager::~ButtonManager()
{
    stop();
}

void ButtonManager::setCallback(ButtonCallback callback)
{
    _callback = std::move(callback);
}

void ButtonManager::start()
{
    if (_taskHandle != nullptr) {
        return;  // Already running
    }

    // Check if any buttons are enabled
    bool anyEnabled = false;
    for (int i = 0; i < 3; ++i) {
        if (!_buttons[i].disabled) {
            anyEnabled = true;
            break;
        }
    }

    if (!anyEnabled) {
        FN_LOGI(TAG, "No buttons enabled, not starting button task");
        return;
    }

    // Stack size needs to accommodate std::function callback and potential logging
    constexpr size_t STACK_SIZE = 4096;
    constexpr int PRIORITY = 1;

    xTaskCreate(
        buttonTask,
        "fnButtons",
        STACK_SIZE,
        this,
        PRIORITY,
        reinterpret_cast<TaskHandle_t*>(&_taskHandle)
    );

    FN_LOGI(TAG, "Button monitoring task started");
}

void ButtonManager::stop()
{
    if (_taskHandle != nullptr) {
        vTaskDelete(reinterpret_cast<TaskHandle_t>(_taskHandle));
        _taskHandle = nullptr;
    }
}

bool ButtonManager::isPressed(ButtonId button) const
{
    int idx = static_cast<int>(button);
    if (_buttons[idx].disabled || _buttons[idx].pin < 0) {
        return false;
    }
    return gpio_get_level(static_cast<gpio_num_t>(_buttons[idx].pin)) == 0;
}

void ButtonManager::buttonTask(void* param)
{
    auto* self = static_cast<ButtonManager*>(param);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Poll every 100ms
        self->pollButtons();
    }
}

void ButtonManager::pollButtons()
{
    // Check each button
    for (int i = 0; i < 3; ++i) {
        ButtonId id = static_cast<ButtonId>(i);
        ButtonEvent event = checkButton(id);

        if (event != ButtonEvent::None && _callback) {
            _callback(id, event);
        }
    }
}

ButtonEvent ButtonManager::checkButton(ButtonId button)
{
    int idx = static_cast<int>(button);
    ButtonState& state = _buttons[idx];

    if (state.disabled) {
        return ButtonEvent::None;
    }

    // Get current time in milliseconds
    unsigned long ms = esp_timer_get_time() / 1000;

    // Read button state (active LOW)
    bool pressed = (gpio_get_level(static_cast<gpio_num_t>(state.pin)) == 0);

    ButtonEvent result = ButtonEvent::None;

    if (pressed) {
        // Button is PRESSED (active LOW)
        if (!state.active) {
            // Just pressed - mark as active and note the time
            state.active = true;
            state.pressStartMs = ms;
        } else {
            // Still pressed - check for long press
            if (state.pressStartMs != IGNORE_TIME &&
                ms - state.pressStartMs > LONG_PRESS_TIME) {
                // Long press detected
                result = ButtonEvent::LongPress;
                // Ignore further activity until release
                state.pressStartMs = IGNORE_TIME;
            }
        }
    } else {
        // Button is NOT pressed (HIGH)
        if (state.active) {
            // Just released
            state.active = false;

            if (state.pressStartMs != IGNORE_TIME) {
                // This was a press-and-release event
                // Check for double tap
                if (state.lastTapMs != 0 && ms - state.lastTapMs < DOUBLE_TAP_TIME) {
                    // Double tap detected
                    state.lastTapMs = 0;
                    result = ButtonEvent::DoubleTap;
                } else {
                    // Store this tap time for potential double tap detection
                    state.lastTapMs = ms;
                }
            }
        } else {
            // Not pressed and wasn't active - check for short press timeout
            if (state.lastTapMs != 0 && ms - state.lastTapMs > DOUBLE_TAP_TIME) {
                // Enough time has passed - this was a short press
                state.lastTapMs = 0;
                result = ButtonEvent::ShortPress;
            }
        }
    }

    return result;
}

} // namespace fujinet::platform::esp32