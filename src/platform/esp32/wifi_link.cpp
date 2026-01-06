#include "fujinet/platform/esp32/wifi_link.h"

// ESP32-only translation unit (built only under ESP-IDF).

extern "C" {
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h" // IPSTR/IP2STR
}

#include "fujinet/core/logging.h"

#include <cstdio>
#include <cstring>

namespace fujinet::platform::esp32 {

static const char* TAG = "nio-wifi";
static constexpr int MAX_RETRIES = 5;

static bool is_same_ssid(const std::string& a, const std::string& b)
{
    return a == b;
}

Esp32WifiLink::Esp32WifiLink() = default;

Esp32WifiLink::~Esp32WifiLink()
{
    // Best-effort cleanup. It is fine if the ESP-IDF state machine is already torn down.
    if (_inited) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_handler);
        (void)esp_wifi_stop();
    }
}

void Esp32WifiLink::init()
{
    if (_inited) {
        return;
    }

    // Requires NVS to be initialized by platform bootstrap.
    {
        // If the TCP/IP stack is already initialized (e.g. by early platform bootstrap),
        // ignore invalid state.
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "esp_netif_init failed: %d", (int)err);
            _state = fujinet::net::LinkState::Failed;
            return;
        }
    }

    // If default loop already exists, ignore invalid state.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "esp_event_loop_create_default failed: %d", (int)err);
        _state = fujinet::net::LinkState::Failed;
        return;
    }

    _netif = esp_netif_create_default_wifi_sta();
    if (!_netif) {
        FN_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        _state = fujinet::net::LinkState::Failed;
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &Esp32WifiLink::event_handler, this, &_wifi_handler));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &Esp32WifiLink::event_handler, this, &_ip_handler));

    _inited = true;
    _state = fujinet::net::LinkState::Disconnected;
    FN_LOGI(TAG, "wifi init ok (STA)");
}

fujinet::net::LinkState Esp32WifiLink::state() const
{
    return _state;
}

void Esp32WifiLink::connect(std::string ssid, std::string pass)
{
    if (!_inited) {
        init();
    }
    if (!_inited) {
        return;
    }

    if ((_state == fujinet::net::LinkState::Connected || _state == fujinet::net::LinkState::Connecting) &&
        is_same_ssid(_ssid, ssid)) {
        return;
    }

    if (_state == fujinet::net::LinkState::Connected && !_ssid.empty() && !is_same_ssid(_ssid, ssid)) {
        disconnect();
    }

    _ssid = std::move(ssid);
    _pass = std::move(pass);
    _retryCount = 0;

    wifi_config_t wifi_cfg{};
    std::memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), _ssid.c_str(), sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), _pass.c_str(), sizeof(wifi_cfg.sta.password) - 1);

    // NOTE: consider relaxing this later (WPA/WPA2/WPA3 mixed environments)
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "esp_wifi_set_config failed: %d", (int)err);
        _state = fujinet::net::LinkState::Failed;
        return;
    }

    _state = fujinet::net::LinkState::Connecting;
    _connectPending = true;
    _userDisconnectRequested = false;

    FN_LOGI(TAG, "connecting to ssid='%s'", _ssid.c_str());

    // Start Wi-Fi if not started yet. We’ll connect on WIFI_EVENT_STA_START.
    err = esp_wifi_start();
    if (err == ESP_OK) {
        return; // wait for STA_START -> on_wifi_start() will connect
    }
    if (err != ESP_ERR_WIFI_NOT_STOPPED) {
        FN_LOGE(TAG, "esp_wifi_start failed: %d", (int)err);
        _state = fujinet::net::LinkState::Failed;
        return;
    }

    // Already started: connect immediately (and treat “already connecting” as OK).
    _wifiStarted = true;
    err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN || err == ESP_ERR_WIFI_STATE) {
        _connectPending = false;
        return;
    }

    FN_LOGE(TAG, "esp_wifi_connect failed: %d", (int)err);
    _state = fujinet::net::LinkState::Failed;
}

void Esp32WifiLink::disconnect()
{
    if (!_inited) {
        return;
    }

    _userDisconnectRequested = true;
    (void)esp_wifi_disconnect();

    _state = fujinet::net::LinkState::Disconnected;
    _connectPending = false;   // if you added this in the earlier fix
    _retryCount = 0;

    _ip.clear();
    _ip_buf[0] = 0;
    _ip_dirty = false;

    FN_LOGI(TAG, "disconnected");
}


void Esp32WifiLink::poll()
{
    // Event-driven; we only promote IP buffer -> std::string here.
    if (_ip_dirty) {
        _ip = std::string(_ip_buf);
        _ip_dirty = false;
    }
}

std::string Esp32WifiLink::ip_address() const
{
    return _ip;
}

void Esp32WifiLink::event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* self = static_cast<Esp32WifiLink*>(arg);
    if (!self) return;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            self->on_wifi_start();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            self->on_wifi_disconnected();
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            self->on_got_ip(static_cast<const ip_event_got_ip_t*>(event_data));
        }
    }
}

void Esp32WifiLink::on_wifi_start()
{
    _wifiStarted = true;

    if (!_connectPending) {
        return;
    }

    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN || err == ESP_ERR_WIFI_STATE) {
        _connectPending = false;
        return;
    }

    FN_LOGE(TAG, "esp_wifi_connect (on start) failed: %d", (int)err);
    _state = fujinet::net::LinkState::Failed;
}

void Esp32WifiLink::on_wifi_disconnected()
{
    if (_userDisconnectRequested) {
        // User-requested; do not auto-retry.
        _userDisconnectRequested = false; // consume it
        _state = fujinet::net::LinkState::Disconnected;
        return;
    }

    if (_retryCount < MAX_RETRIES && !_ssid.empty()) {
        _retryCount++;
        FN_LOGW(TAG, "wifi disconnected; retry %d/%d (ssid='%s')", _retryCount, MAX_RETRIES, _ssid.c_str());
        _state = fujinet::net::LinkState::Connecting;
        _connectPending = true;           // if using the earlier approach
        (void)esp_wifi_connect();         // OK to ignore “already connecting” errors
        return;
    }

    FN_LOGE(TAG, "wifi disconnected; giving up (ssid='%s')", _ssid.c_str());
    _state = fujinet::net::LinkState::Failed;
}


void Esp32WifiLink::on_got_ip(const ip_event_got_ip_t* ev)
{
    if (!ev) {
        return;
    }

    // Format IPv4
    std::snprintf(_ip_buf, sizeof(_ip_buf), IPSTR, IP2STR(&ev->ip_info.ip));
    _ip_dirty = true;

    _retryCount = 0;
    _state = fujinet::net::LinkState::Connected;
    FN_LOGI(TAG, "got ip: %s", _ip_buf);
}

} // namespace fujinet::platform::esp32

