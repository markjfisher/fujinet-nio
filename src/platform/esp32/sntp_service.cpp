#include "fujinet/platform/esp32/sntp_service.h"
#include "fujinet/core/logging.h"

extern "C" {
#include "esp_sntp.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "service";

SntpService* SntpService::_instance = nullptr;

SntpService::SntpService(fujinet::core::SystemEvents& events)
    : _events(events)
{
    _instance = this;

    _sub = _events.network().subscribe([this](const fujinet::net::NetworkEvent& ev) {
        this->on_network_event(ev);
    });
}

SntpService::~SntpService()
{
    stop();
    _events.network().unsubscribe(_sub);
    if (_instance == this) _instance = nullptr;
}

void SntpService::stop()
{
    if (_started) {
        esp_sntp_stop();
        _started = false;
    }
}

void SntpService::on_network_event(const fujinet::net::NetworkEvent& ev)
{
    if (ev.kind != fujinet::net::NetworkEventKind::GotIp) return;
    if (_started) return;

    // Minimal default configuration; can be moved to config later.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(&SntpService::on_time_sync_cb);

    esp_sntp_init();
    _started = true;

    FN_LOGI(TAG, "started (server=pool.ntp.org)");
}

void SntpService::on_time_sync_cb(struct timeval* /*tv*/)
{
    if (!_instance) return;

    FN_LOGI(TAG, "time synchronized");

    fujinet::time::TimeEvent tev;
    tev.kind = fujinet::time::TimeEventKind::Synchronized;
    tev.synced.source = "sntp";

    _instance->_events.time().publish(tev);
}

} // namespace fujinet::platform::esp32
