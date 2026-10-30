#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_system.h" // esp_restart


extern "C" {
#include "nvs_flash.h"
}

#include "fujinet/build/profile.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/net/network_link_monitor.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/early_init.h"
#include "fujinet/platform/esp32/button_manager.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/led_manager.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/platform/esp32/sntp_service.h"
#include "fujinet/platform/esp32/wifi_link.h"
#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
#include "fujinet/diag/diagnostic_atari_sio.h"
#include "fujinet/platform/esp32/atari_sio_ingress.h"
#endif
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/core/logging.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/diag/diagnostic_registry.h"
#include "fujinet/fs/boot_mount.h"
#include "fujinet/hotsync/hotsync_init.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

static constexpr const char* TAG = "nio";

using namespace fujinet;
using namespace fujinet::io::protocol;

namespace {
struct Esp32Services {
    fujinet::io::FujiDevice* fuji{nullptr};

    fujinet::core::SystemEvents* events{nullptr};

    std::unique_ptr<fujinet::net::NetworkLinkMonitor> wifiMon;

    // Starts SNTP when NetworkEvent::GotIp occurs.
    std::unique_ptr<fujinet::platform::esp32::SntpService> sntp;

    // Button manager for physical buttons (reset, etc.)
    std::unique_ptr<fujinet::platform::esp32::ButtonManager> buttons;

    // LED manager for status LEDs
    std::unique_ptr<fujinet::platform::esp32::LedManager> leds;

    bool phase1_started{false};

    // Wi-Fi bring-up (esp_wifi_init, starting the radio) takes a second or more. It must not run
    // on the core task: that task also serves SIO, and a stalled SIO task misses the Atari's boot
    // commands. A low-priority task builds and connects the link, then publishes it here; until
    // then the Wi-Fi service answers "not ready" at once.
    std::atomic<fujinet::platform::esp32::Esp32WifiLink*> wifi_ready{nullptr};
    std::atomic<bool> wifi_starting{false};

    struct WifiStartJob {
        Esp32Services* services;
        bool connect;
        std::string ssid;
        std::string pass;
        std::string bssid;
    };

    static void wifi_start_task(void* arg)
    {
        std::unique_ptr<WifiStartJob> job(static_cast<WifiStartJob*>(arg));
        auto link = std::make_unique<fujinet::platform::esp32::Esp32WifiLink>();
        const auto begun_us = esp_timer_get_time();
        link->init();
        if (job->connect) {
            link->set_bssid(job->bssid);
            link->connect(job->ssid, job->pass);
        }
        FN_ELOG("[%u ms] wifi bring-up done in %u ms", (unsigned)(esp_timer_get_time()/1000),
                (unsigned)((esp_timer_get_time() - begun_us)/1000));
        // Published for good: the link lives as long as the firmware.
        job->services->wifi_ready.store(link.release(), std::memory_order_release);
        vTaskDelete(nullptr);
    }

    void start_wifi_async(bool connect_from_config)
    {
        if (!events || wifi_starting.exchange(true)) return;

        auto* job = new WifiStartJob{this, false, {}, {}, {}};
        if (connect_from_config && fuji) {
            const auto& cfg = fuji->config();
            if (cfg.wifi.enabled && !cfg.wifi.ssid.empty()) {
                job->connect = true;
                job->ssid = cfg.wifi.ssid;
                job->pass = cfg.wifi.passphrase;
                job->bssid = cfg.wifi.bssid;
            }
        }
        // Priority 1, below the core task (5), and on core 0 with the Wi-Fi driver, away from the SIO task.
        if (xTaskCreatePinnedToCore(&wifi_start_task, "fn_wifi_start", 6144, job, 1, nullptr, 0) != pdPASS) {
            FN_LOGE(TAG, "Could not start the Wi-Fi bring-up task");
            delete job;
            wifi_starting.store(false);
        }
    }

    // The link once it is up, otherwise nullptr. A request that needs Wi-Fi before phase 1 had
    // configured it starts the bring-up in the background and is answered "not ready".
    fujinet::net::INetworkLink* ensure_wifi(bool connect_from_config = false)
    {
        auto* link = wifi_ready.load(std::memory_order_acquire);
        // Before phase 1 has decided whether to connect, only report "not up yet".
        if (!link && (phase1_started || connect_from_config)) start_wifi_async(connect_from_config);
        return link;
    }

    void init_phase0(fujinet::core::FujinetCore& core)
    {
        events = &core.events();
        // Construct services that subscribe to events (even if Wi-Fi may be disabled).
        sntp = std::make_unique<fujinet::platform::esp32::SntpService>(*events);

        // Initialize button manager with reset callback
        buttons = std::make_unique<fujinet::platform::esp32::ButtonManager>();
        buttons->setCallback([](fujinet::platform::esp32::ButtonId id,
                                fujinet::platform::esp32::ButtonEvent event) {
            using namespace fujinet::platform::esp32;
            if (id == ButtonId::C && event == ButtonEvent::ShortPress) {
                FN_LOGI(TAG, "Button C: SHORT PRESS - Rebooting");
                esp_restart();
            }
            // Log other button events for debugging
            const char* buttonName = (id == ButtonId::A) ? "A" :
                                     (id == ButtonId::B) ? "B" : "C";
            const char* eventName = (event == ButtonEvent::ShortPress) ? "SHORT_PRESS" :
                                    (event == ButtonEvent::LongPress) ? "LONG_PRESS" :
                                    (event == ButtonEvent::DoubleTap) ? "DOUBLE_TAP" : "NONE";
            FN_LOGI(TAG, "Button %s: %s", buttonName, eventName);
        });
        buttons->start();

        // Initialize LED manager
        leds = std::make_unique<fujinet::platform::esp32::LedManager>();
        leds->setup();
        // GPIO 2 drives the white power indicator on Atari v1; leave it on.
        leds->set(fujinet::platform::esp32::LedId::Wifi, true);
    }

    void start_phase1(fujinet::core::FujinetCore& core)
    {
        if (phase1_started || !fuji || !events) return;
        phase1_started = true;

        // Config already loaded during transport setup, just get reference
        const auto& cfg = fuji->config();

        if (cfg.wifi.enabled && !cfg.wifi.ssid.empty()) {
            ensure_wifi(true);
        }

        fujinet::core::register_wifi_service(
            core, fuji->config_mut(), fuji->config_store(),
            [this]() { return ensure_wifi(false); });

        // start all the devices that can be delayed
        // We can now check if they should be started too
        // Register clock device with config store for timezone persistence
        fujinet::core::register_clock_device(core, fuji ? fuji->config_store() : nullptr);
        fujinet::core::register_network_device(core, cfg.translation);
        if (cfg.modem.enabled)
            fujinet::core::register_modem_device(core);
    }

    void poll()
    {
        auto* link = wifi_ready.load(std::memory_order_acquire);
        if (!link) return;
        // Subscribes to the event bus, so it is created here, on the core task, not by the bring-up.
        if (!wifiMon) wifiMon = std::make_unique<fujinet::net::NetworkLinkMonitor>(*events, *link);
        link->poll();
        wifiMon->poll();
    }
};
} // namespace
    

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;
    Esp32Services services;

    // Diagnostics + console (cooperative; keep in the core task to avoid races).
    fujinet::diag::DiagnosticRegistry diagRegistry;
    auto coreDiag = fujinet::diag::create_core_diagnostic_provider(core);
    auto wifiDiagCtx = std::make_shared<fujinet::diag::NetworkDiagWifiContext>();
    auto netDiag  = fujinet::diag::create_network_diagnostic_provider(core, wifiDiagCtx);
    auto diskDiag = fujinet::diag::create_disk_diagnostic_provider(core);
    auto modemDiag = fujinet::diag::create_modem_diagnostic_provider(core);
    auto appStoreDiag = fujinet::diag::create_app_store_diagnostic_provider(core);
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> uartChannelDiag;
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> packetLinkDiag;
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> bootDiag;
    diagRegistry.add_provider(*coreDiag);
    diagRegistry.add_provider(*netDiag);
    diagRegistry.add_provider(*diskDiag);
    diagRegistry.add_provider(*modemDiag);
    diagRegistry.add_provider(*appStoreDiag);

    std::unique_ptr<fujinet::console::IConsoleTransport> consoleTransport;
    std::unique_ptr<fujinet::console::ConsoleEngine> console;
    bool console_running = false;

#if CONFIG_FN_CONSOLE_ENABLE
    consoleTransport = fujinet::console::create_default_console_transport();
    if (consoleTransport) {
        console = std::make_unique<fujinet::console::ConsoleEngine>(diagRegistry, *consoleTransport, core.storageManager());
        console->set_reboot_handler([]{
            esp_restart();
        });
        console_running = true;
        consoleTransport->write_line("fujinet-nio diagnostic console (type: help)");
#if CONFIG_FN_CONSOLE_TRANSPORT_UART0
        FN_LOGI(TAG, "Console started (UART0)");
#endif
    }
#endif

    services.init_phase0(core);
    
    if (auto flashFs = platform::esp32::create_flash_filesystem()) {
        if (!core.storageManager().registerFileSystem(std::move(flashFs))) {
            FN_LOGE(TAG, "StorageManager refused to register 'flash' filesystem");
        } else {
            FN_LOGI(TAG, "Flash filesystem registered as 'flash'");
        }
    }

    if (auto sdFs = platform::esp32::create_sdcard_filesystem()) {
        if (!core.storageManager().registerFileSystem(std::move(sdFs))) {
            FN_LOGE(TAG, "StorageManager refused to register 'sd0' filesystem");
        } else {
            FN_LOGI(TAG, "SD filesystem registered as 'sd0'");
        }
    }

    // Register TNFS filesystem provider. Endpoint/transport are resolved from URI at access time.
    if (auto tnfsFs = platform::esp32::create_tnfs_filesystem()) {
        if (!core.storageManager().registerFileSystem(std::move(tnfsFs))) {
            FN_LOGE(TAG, "StorageManager refused to register 'tnfs' filesystem");
        } else {
            FN_LOGI(TAG, "TNFS filesystem registered as 'tnfs' (dynamic URI endpoints)");
        }
    }

    auto profile = build::current_build_profile();

    {
        auto dev = platform::create_fuji_device(core, profile);

        // Keep a non-owning pointer for phase-1 start.
        if (auto* fuji = dynamic_cast<fujinet::io::FujiDevice*>(dev.get())) {
            services.fuji = fuji;
            wifiDiagCtx->fuji = fuji;
            wifiDiagCtx->ensure_wifi = [&services]() {
                return services.ensure_wifi(false);
            };
        } else {
            FN_LOGE(TAG, "create_fuji_device() did not return a FujiDevice; Wi-Fi/config start disabled");
            services.fuji = nullptr;
        }

        io::DeviceID fujiDeviceId = to_device_id(WireDeviceId::FujiNet);
        
        FN_ELOG("Registering FujiDevice on DeviceID %u", static_cast<unsigned>(fujiDeviceId));
        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %u",
                static_cast<unsigned>(fujiDeviceId));
        }
    }

    // Load config immediately - transports need it (especially NetSIO config)
    // This is fast (just reading a YAML file) and necessary for proper transport setup
    if (services.fuji) {
        FN_ELOG("[FujiDevice] Loading config for transport setup");
        services.fuji->start();
    }
    const auto& config = services.fuji ? services.fuji->config() : fujinet::config::FujiConfig{};

    if (auto httpFs = platform::esp32::create_http_filesystem()) {
        if (!core.storageManager().registerFileSystem(std::move(httpFs))) {
            FN_LOGE(TAG, "StorageManager refused to register 'http' filesystem");
        } else {
            FN_LOGI(TAG, "HTTP filesystem registered as 'http' (dynamic URL endpoints)");
        }
    }

    // Atari disk SIO is a direct DiskService ingress, not a Channel/transport.
    std::unique_ptr<fujinet::io::Channel> channel;
    if (profile.primaryTransport != fujinet::build::TransportKind::None) {
        channel = platform::create_channel_for_profile(profile, config);
    }
    if (profile.primaryTransport != fujinet::build::TransportKind::None && !channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    if (channel) {
        uartChannelDiag = fujinet::diag::create_uart_channel_diagnostic_provider(channel.get(), services.fuji);
        if (uartChannelDiag) diagRegistry.add_provider(*uartChannelDiag);
        packetLinkDiag = fujinet::diag::create_packet_link_diagnostic_provider(channel.get(), services.fuji);
        if (packetLinkDiag) diagRegistry.add_provider(*packetLinkDiag);
    }

    // Palm HotSync (FN_WITH_HOTSYNC builds). With hotsync.cradle on it puts
    // the cradle over `channel`, so the transports below get the cradle; the
    // UART diagnostics above keep the line beneath it.
    auto hotsync = fujinet::hotsync::start_hotsync(
        {core.storageManager(), config.hotsync, services.fuji, diagRegistry}, channel);

    if (channel) {
        core::setup_transports(core, *channel, profile, &config);
        FN_ELOG("transport initialized");
    }

    // Not tied to a Channel: the Atari SIO profile has none, and still needs boot.* on the console.
    bootDiag = fujinet::diag::create_boot_diagnostic_provider(services.fuji);
    if (bootDiag) diagRegistry.add_provider(*bootDiag);

    // If we load config at this point to find out if the services should be enabled or not, it adds 80ms before the main loop starts
    fujinet::core::register_host_service(core);
    fujinet::core::register_file_device(core);
    fujinet::core::register_application_state_services(core);
    fujinet::core::register_disk_device(core);

#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
    std::unique_ptr<fujinet::platform::esp32::AtariSioIngress> atariSioIngress;
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> atariSioDiag;
#endif
    // Restore live drive mappings and then apply the configured boot disk.
    // This must happen after DiskDevice is registered.
    {
        fujinet::io::DeviceID diskDeviceId = fujinet::io::protocol::to_device_id(fujinet::io::protocol::WireDeviceId::DiskService);
        auto* diskDev = dynamic_cast<fujinet::io::DiskDevice*>(
            core.deviceManager().getDevice(diskDeviceId));
        if (diskDev) {
            constexpr std::size_t activeBootDiskUnit = 0;
            if (config.boot.mode == fujinet::config::BootMode::Config) {
                diskDev->configure_boot_mount(config.boot.configUri, config.boot.readOnly);
            }
            diskDev->restore_runtime_mounts();
            std::size_t bootApplied = fujinet::apply_boot_mount(
                    diskDev->disk_service(),
                    core.storageManager(),
                    config.boot,
                    activeBootDiskUnit);
            if (bootApplied) {
            }
            FN_LOGI(TAG, "Applied %zu boot config mount", bootApplied);

#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
            if (profile.primaryChannel == fujinet::build::ChannelKind::SioGpio) {
                atariSioIngress = std::make_unique<fujinet::platform::esp32::AtariSioIngress>(
                    config, diskDev->disk_service(), core.routingManager());
                core.addWorkSource(atariSioIngress.get());
                // The ingress has no logging (SIO timing is tight): sio.status shows its counters.
                auto* ingress = atariSioIngress.get();
                atariSioDiag = fujinet::diag::create_atari_sio_diagnostic_provider(
                    [ingress] { return ingress->stats(); },
                    [ingress] { ingress->reset_stats(); },
                    [ingress](std::uint32_t target) { return ingress->probe_baud(target); },
                    [ingress](int index, bool fallback) { return ingress->set_hsio_index(index, fallback); },
                    [ingress](const fujinet::io::AtariSioTuning& tuning) { ingress->set_tuning(tuning); },
                    [fuji = services.fuji](int hsioIndex, const fujinet::io::AtariSioTuning& tuning) -> std::string {
                        if (!fuji) return "FujiDevice not available";
                        auto* store = fuji->config_store();
                        if (!store) return "config store not available";
                        auto& saved = fuji->config_mut().atariSio;
                        saved.hsioIndex = hsioIndex;
                        saved.ackGapUs = tuning.ack_gap_us;
                        saved.dataGapUs = tuning.data_gap_us;
                        saved.altWindowMs = tuning.alt_window_us / 1000;
                        saved.ignoreWriteGlitches = tuning.ignore_write_glitches;
                        saved.procNotify = tuning.proc_notify;
                        store->save(fuji->config());
                        return {};
                    },
                    [ingress](bool asserted) { return ingress->set_proc(asserted); },
                    [ingress](unsigned count, unsigned lowMs, unsigned highMs) {
                        return ingress->pulse_proc(count, lowMs, highMs);
                    });
                if (atariSioDiag) diagRegistry.add_provider(*atariSioDiag);
            }
#endif

        } else {
            FN_LOGW(TAG, "Could not get DiskDevice to restore runtime or boot mounts");
        }
    }

    const std::uint64_t phase1_at = core.tick_count() + 100;
    const auto idleDelay = std::chrono::milliseconds(20);
    
    FN_ELOG("[%u ms] starting main loop", (unsigned)(esp_timer_get_time()/1000));

    // Run the core loop forever.
    for (;;) {
#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
        if (atariSioIngress) atariSioIngress->poll();
#endif
        core.tick();

        // start phase-1 services after a small delay
        if (!services.phase1_started && core.tick_count() >= phase1_at) {
            // Timed because it runs on the SIO task: whatever it takes, SIO is not served meanwhile.
            const auto phase1_start_us = esp_timer_get_time();
            services.start_phase1(core);
            FN_ELOG("[%u ms] phase 1 done in %u ms", (unsigned)(esp_timer_get_time()/1000),
                    (unsigned)((esp_timer_get_time() - phase1_start_us)/1000));
        }

        services.poll();

        if (console_running) {
            console_running = console->step(0);
            if (!console_running) {
                FN_LOGW(TAG, "Console stopped: it will not respond until restart");
            }
        }

// Do this later when we want to check the water mark
// #if defined(FN_DEBUG)
//         if (core.tick_count() % 100 == 0) {
//             UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
//             // hw is MINIMUM free stack (in words) since task start
//             FN_LOGD(TAG, "fujinet_core stack high-water mark: %u words free", hw);
//         }
// #endif

#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
        if (atariSioIngress) {
            atariSioIngress->wait_for_work(idleDelay);
        } else
#endif
        if (core.hasWaitableWorkSource()) {
            core.waitForWork(idleDelay);
        } else {
            vTaskDelay(pdMS_TO_TICKS(idleDelay.count()));
        }
    }
    FN_LOGI(TAG, "core task exiting");
}

extern "C" void app_main(void)
{
    // No logging before early_init: console and log backend are board-dependent (UART or USB CDC).
    fujinet::platform::early_init();

    // Global default: be strict
    esp_log_level_set("*", ESP_LOG_ERROR);

    // Turn our own tags back up a bit
    esp_log_level_set("button",      ESP_LOG_INFO);
    esp_log_level_set("clock",       ESP_LOG_INFO);
    esp_log_level_set("config",      ESP_LOG_INFO);
    esp_log_level_set("console",     ESP_LOG_INFO);
    esp_log_level_set("core",        ESP_LOG_INFO);
    esp_log_level_set("disk",        ESP_LOG_INFO);
    // One ~250 byte line per sector request (see disk.stats for the same counters on demand), so it is
    // off by default; raise it to ESP_LOG_INFO here to watch the counters move.
    esp_log_level_set("diskstats",   ESP_LOG_WARN);
    esp_log_level_set("fs",          ESP_LOG_INFO);
    esp_log_level_set("fujibus",     ESP_LOG_INFO);
    esp_log_level_set("io",          ESP_LOG_INFO);
    esp_log_level_set("led",         ESP_LOG_INFO);
    esp_log_level_set("mount",       ESP_LOG_INFO);
    esp_log_level_set("nio",         ESP_LOG_INFO);
    esp_log_level_set("nio-wifi",    ESP_LOG_INFO);
    esp_log_level_set("platform",    ESP_LOG_INFO);
    esp_log_level_set("service",     ESP_LOG_INFO);
    esp_log_level_set("tcp",         ESP_LOG_INFO);
    esp_log_level_set("tls_diag",    ESP_LOG_INFO);
    esp_log_level_set("tnfs_fs",     ESP_LOG_INFO);
    esp_log_level_set("uart_ch",     ESP_LOG_INFO);
    esp_log_level_set("udp",         ESP_LOG_INFO);
    
    // legacy testing
    esp_log_level_set("l_byte",      ESP_LOG_INFO);
    esp_log_level_set("l_iwm",       ESP_LOG_INFO);
    esp_log_level_set("l_iwm_hw",    ESP_LOG_INFO);
    esp_log_level_set("l_netadpt",   ESP_LOG_INFO);
    esp_log_level_set("l_netsio",    ESP_LOG_INFO);
    esp_log_level_set("l_packet",    ESP_LOG_INFO);
    esp_log_level_set("l_sio",       ESP_LOG_INFO);
    esp_log_level_set("l_sio_hw",    ESP_LOG_INFO);

    // Silence noisy ESP components we care about:
    esp_log_level_set("heap_init",   ESP_LOG_ERROR);
    esp_log_level_set("spi_flash",   ESP_LOG_ERROR);
    esp_log_level_set("sleep_gpio",  ESP_LOG_ERROR);
    esp_log_level_set("app_init",    ESP_LOG_ERROR);
    esp_log_level_set("efuse_init",  ESP_LOG_ERROR);
    esp_log_level_set("octal_psram", ESP_LOG_ERROR);
    esp_log_level_set("cpu_start",   ESP_LOG_ERROR);
    esp_log_level_set("main_task",   ESP_LOG_ERROR);
    esp_log_level_set("wifi",        ESP_LOG_ERROR);

    // TinyUSB glue:
    esp_log_level_set("tusb_desc",   ESP_LOG_ERROR);
    esp_log_level_set("TinyUSB",     ESP_LOG_ERROR);

    FN_ELOG("fujinet-nio - starting up");

    // Platform bootstrap: NVS init (required by Wi-Fi and other ESP-IDF subsystems).
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            FN_LOGW(TAG, "NVS init needs erase (err=%d), erasing", (int)err);
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            FN_LOGE(TAG, "nvs_flash_init failed: %d", (int)err);
            // Continue boot; Wi-Fi will fail later if requested.
        }
    }

    // Initialize the TCP/IP stack + default event loop early so network protocols
    // cannot crash if invoked before Wi-Fi link bring-up (phase-1).
    {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "esp_netif_init failed: %d", (int)err);
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "esp_event_loop_create_default failed: %d", (int)err);
        }
    }

    if (!fujinet::platform::esp32::init_littlefs()) {
        FN_LOGE(TAG, "Failed to initialise LittleFS; config persistence will not work.");
        // ... what to do?
    }

    if (!fujinet::platform::esp32::init_sdcard_spi()) {
        FN_LOGE(TAG, "Failed to initialise SD card.");
        // ... what to do?
    }

    // Core 1: the Wi-Fi driver task is pinned to core 0, and sharing a core with the radio costs the
    // SIO task part of its 16 ms ACK window. The CMD interrupt and the SIO UART are set up by this
    // task, so they land on core 1 too.
    xTaskCreatePinnedToCore(
        &fujinet_core_task,
        "fujinet_core",
        // 4176, // would like this to be 4096, but yaml-cpp saving blows up. Might need to think about its long term usage!
        // 4176 was as low as I could go without it blowing up, but that wouldn't give us much room for anything else, so simply doubling to 8k for now
        8192,
        nullptr,
        5,
        nullptr,
        1
    );
}
