#include "fujinet/platform/esp32/led_manager.h"
#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/core/logging.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <atomic>

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "led";

// Blink duration in milliseconds
static constexpr int BLINK_TIME_MS = 100;
static std::atomic<std::uint32_t> network_events{0};

void note_network_activity() noexcept
{
    network_events.fetch_add(1, std::memory_order_relaxed);
}

void note_activity() noexcept
{
    note_network_activity();
}

void LedManager::activity_tick(void* arg)
{
    auto* self = static_cast<LedManager*>(arg);
    const bool on = self->_activity_pulse.tick(network_events.load(std::memory_order_relaxed));
    if (self->isOn(LedId::Bus) != on) self->set(LedId::Bus, on);
}

LedManager::~LedManager()
{
    if (_activity_timer) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(_activity_timer));
        esp_timer_delete(static_cast<esp_timer_handle_t>(_activity_timer));
    }
}

LedManager::LedManager()
{
    const PinMap& pins = pinmap();

    // Initialize pin array from pinmap
    _pin[static_cast<int>(LedId::Wifi)] = pins.led.wifi;
    _pin[static_cast<int>(LedId::Bus)] = pins.led.bus;
    _pin[static_cast<int>(LedId::Bluetooth)] = pins.led.bluetooth;
}

void LedManager::setup()
{
    if (_initialized) {
        return;
    }

    // Configure GPIO for each enabled LED
    for (int i = 0; i < 3; ++i) {
        if (_pin[i] >= 0) {
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_INPUT_OUTPUT;  // Need input mode for reading back state
            io_conf.pin_bit_mask = (1ULL << _pin[i]);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

            esp_err_t err = gpio_config(&io_conf);
            if (err != ESP_OK) {
                FN_LOGE(TAG, "Failed to configure LED %d GPIO %d: %d", i, _pin[i], err);
                _pin[i] = -1;  // Disable this LED
            } else {
                // LEDs are active LOW, so set HIGH to turn off initially
                gpio_set_level(static_cast<gpio_num_t>(_pin[i]), 1);
                _state[i] = false;
                FN_LOGI(TAG, "LED %d enabled on GPIO %d", i, _pin[i]);
            }
        }
    }

    _initialized = true;
    if (pinmap().led.network_activity && _pin[static_cast<int>(LedId::Bus)] >= 0) {
        esp_timer_create_args_t args{};
        args.callback = &LedManager::activity_tick;
        args.arg = this;
        args.name = "network_led";
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            _activity_timer = timer;
            if (esp_timer_start_periodic(timer, 25000) != ESP_OK) {
                esp_timer_delete(timer);
                _activity_timer = nullptr;
            }
        }
        if (!_activity_timer) FN_LOGW(TAG, "Could not start activity LED timer");
    }
}

void LedManager::set(LedId id, bool on)
{
    int idx = static_cast<int>(id);

    if (!_initialized || _pin[idx] < 0) {
        return;
    }

    _state[idx] = on;

    // LEDs are active LOW: set LOW to turn on, HIGH to turn off
    gpio_set_level(static_cast<gpio_num_t>(_pin[idx]), on ? 0 : 1);
}

void LedManager::toggle(LedId id)
{
    int idx = static_cast<int>(id);

    if (!_initialized || _pin[idx] < 0) {
        return;
    }

    set(id, !_state[idx]);
}

void LedManager::blink(LedId id, int count)
{
    int idx = static_cast<int>(id);

    if (!_initialized || _pin[idx] < 0 || count <= 0) {
        return;
    }

    // Save original state
    bool originalState = _state[idx];

    for (int i = 0; i < count; ++i) {
        set(id, true);
        vTaskDelay(pdMS_TO_TICKS(BLINK_TIME_MS));
        set(id, false);
        vTaskDelay(pdMS_TO_TICKS(BLINK_TIME_MS));
    }

    // Restore original state
    set(id, originalState);
}

bool LedManager::isOn(LedId id) const
{
    int idx = static_cast<int>(id);
    return _state[idx];
}

} // namespace fujinet::platform::esp32
