#include "fujinet/platform/esp32/wifi_link.h"

// ESP32-only translation unit (built only under ESP-IDF).

extern "C" {
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h" // IPSTR/IP2STR
}

#include "fujinet/core/logging.h"

#include <cstdio>
#include <cstring>

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "nio-wifi";
static constexpr int MAX_RETRIES = 5;

static const char* auth_mode_label(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:           return "open";
    case WIFI_AUTH_WEP:            return "wep";
    case WIFI_AUTH_WPA_PSK:        return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:       return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:   return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:       return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:  return "wpa2_wpa3_psk";
    case WIFI_AUTH_WAPI_PSK:       return "wapi_psk";
    default:                       return "unknown";
    }
}

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

void Esp32WifiLink::prepare_for_new_connection()
{
    if (!_inited) {
        return;
    }

    // Suppress auto-retry while tearing down an in-flight or failed attempt.
    _userDisconnectRequested = true;
    _connectPending = false;

    if (_wifiStarted) {
        (void)esp_wifi_disconnect();
        (void)wait_link_state(fujinet::net::LinkState::Disconnected, 3000);

        const esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT) {
            FN_LOGW(TAG, "esp_wifi_stop failed: %d", (int)stop_err);
        }
        _wifiStarted = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    _userDisconnectRequested = false;
    _retryCount = 0;
    _state = fujinet::net::LinkState::Disconnected;
    _ip.clear();
    _ip_buf[0] = 0;
    _ip_dirty = false;
}

bool Esp32WifiLink::try_wifi_connect()
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        const esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            _connectPending = false;
            return true;
        }

        FN_LOGW(TAG, "esp_wifi_connect attempt %d failed: %s", attempt + 1, esp_err_to_name(err));

        if (err == ESP_ERR_WIFI_CONN || err == ESP_ERR_WIFI_STATE) {
            (void)esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        break;
    }

    _connectPending = false;
    _state = fujinet::net::LinkState::Failed;
    return false;
}

void Esp32WifiLink::connect(std::string ssid, std::string pass)
{
    if (!_inited) {
        init();
    }
    if (!_inited) {
        return;
    }

    const bool same_creds = is_same_ssid(_ssid, ssid) && _pass == pass;

    if (_state == fujinet::net::LinkState::Connected && same_creds) {
        return;
    }

    if (_state == fujinet::net::LinkState::Connecting && same_creds) {
        return;
    }

    const bool need_teardown =
        _state == fujinet::net::LinkState::Connected ||
        _state == fujinet::net::LinkState::Connecting ||
        _state == fujinet::net::LinkState::Failed ||
        (_wifiStarted && !same_creds);

    if (need_teardown) {
        prepare_for_new_connection();
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

    // Prefer the STA_START path after a stop/start teardown; fall back if already running.
    err = esp_wifi_start();
    if (err == ESP_OK) {
        return; // wait for STA_START -> on_wifi_start() will connect
    }
    if (err != ESP_ERR_WIFI_NOT_STOPPED) {
        FN_LOGE(TAG, "esp_wifi_start failed: %d", (int)err);
        _state = fujinet::net::LinkState::Failed;
        _connectPending = false;
        return;
    }

    _wifiStarted = true;
    if (!try_wifi_connect()) {
        FN_LOGE(TAG, "esp_wifi_connect failed after start");
    }
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

bool Esp32WifiLink::wait_wifi_started(int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 50) {
        if (_wifiStarted) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return _wifiStarted;
}

bool Esp32WifiLink::wait_link_state(fujinet::net::LinkState target, int timeout_ms)
{
    using fujinet::net::LinkState;
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 50) {
        if (_state == target) {
            return true;
        }
        if (target == LinkState::Disconnected &&
            (_state == LinkState::Disconnected || _state == LinkState::Failed)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return _state == target;
}

esp_err_t Esp32WifiLink::start_scan_with_retries(bool allow_disconnect_for_scan, bool& disconnected_for_scan)
{
    disconnected_for_scan = false;

    wifi_scan_config_t scan_cfg{};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 120;
    scan_cfg.scan_time.active.max = 300;

    for (int attempt = 0; attempt < 5; ++attempt) {
        const esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        FN_LOGW(TAG, "esp_wifi_scan_start attempt %d failed: %s", attempt + 1, esp_err_to_name(err));

        if (err == ESP_ERR_WIFI_NOT_STARTED) {
            (void)esp_wifi_start();
            (void)wait_wifi_started(2000);
            continue;
        }

        if (err == ESP_ERR_WIFI_STATE || err == ESP_FAIL) {
            if (_state == fujinet::net::LinkState::Connecting) {
                if (!disconnected_for_scan && allow_disconnect_for_scan) {
                    FN_LOGI(TAG, "pausing Wi-Fi connection for scan");
                    _userDisconnectRequested = true;
                    (void)esp_wifi_disconnect();
                    (void)wait_link_state(fujinet::net::LinkState::Disconnected, 3000);
                    disconnected_for_scan = true;
                    _userDisconnectRequested = false;
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
        }

        return err;
    }

    return ESP_FAIL;
}

WifiScanResult Esp32WifiLink::scan()
{
    WifiScanResult result;

    if (!_inited) {
        init();
    }
    if (!_inited) {
        result.error = "Wi-Fi init failed";
        return result;
    }

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        result.error = std::string("esp_wifi_start failed: ") + esp_err_to_name(err);
        return result;
    }

    if (!wait_wifi_started(3000)) {
        result.error = "timed out waiting for Wi-Fi radio start";
        return result;
    }

    const bool should_reconnect =
        (_state == fujinet::net::LinkState::Connected || _state == fujinet::net::LinkState::Connecting) &&
        !_ssid.empty();

    bool disconnected_for_scan = false;
    err = start_scan_with_retries(should_reconnect, disconnected_for_scan);
    if (err != ESP_OK) {
        result.error = std::string("esp_wifi_scan_start failed: ") + esp_err_to_name(err);
        if (err == ESP_ERR_WIFI_STATE) {
            result.error += " (STA was connecting; try again after net.wifi.status shows connected or failed)";
        }
        return result;
    }

    std::uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK) {
        result.error = std::string("esp_wifi_scan_get_ap_num failed: ") + esp_err_to_name(err);
        return result;
    }

    if (count == 0) {
        result.success = true;
        return result;
    }

    std::vector<wifi_ap_record_t> records(count);
    err = esp_wifi_scan_get_ap_records(&count, records.data());
    if (err != ESP_OK) {
        result.error = std::string("esp_wifi_scan_get_ap_records failed: ") + esp_err_to_name(err);
        return result;
    }

    result.aps.reserve(count);
    for (const auto& rec : records) {
        WifiScanAp ap;
        ap.ssid = std::string(reinterpret_cast<const char*>(rec.ssid));
        ap.rssi = rec.rssi;
        ap.channel = rec.primary;
        ap.auth = auth_mode_label(rec.authmode);
        result.aps.push_back(std::move(ap));
    }

    result.success = true;
    FN_LOGI(TAG, "scan found %u access points", static_cast<unsigned>(result.aps.size()));

    if (disconnected_for_scan && should_reconnect) {
        _userDisconnectRequested = false;
        _retryCount = 0;
        _state = fujinet::net::LinkState::Connecting;
        _connectPending = true;
        (void)esp_wifi_connect();
    }

    return result;
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

    if (!try_wifi_connect()) {
        FN_LOGE(TAG, "esp_wifi_connect (on start) failed");
    }
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
    _connectPending = false;
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

