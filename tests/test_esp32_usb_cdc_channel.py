"""Run the real ESP USB channel and PacketLink against a tiny host USB fake."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    "sdkconfig.h": """
#define CONFIG_TINYUSB_CDC_ENABLED 1
#define CONFIG_FN_FUJIBUS_TRANSPORT_USB_CDC 1
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
""",
    "freertos/task.h": """
#pragma once
#include "FreeRTOS.h"
inline TickType_t xTaskGetTickCount() { static TickType_t tick; return ++tick; }
inline void vTaskDelay(TickType_t) {}
""",
    "tinyusb.h": "",
    "tinyusb_default_config.h": "",
    "tusb.h": """
#pragma once
#include <stdint.h>
uint32_t tud_cdc_n_available(uint8_t itf);
""",
    "tinyusb_cdc_acm.h": """
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { TINYUSB_CDC_ACM_0, TINYUSB_CDC_ACM_1 } tinyusb_cdcacm_itf_t;
esp_err_t tinyusb_cdcacm_read(tinyusb_cdcacm_itf_t, uint8_t*, size_t, size_t*);
size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t, const uint8_t*, size_t);
esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t, uint32_t);
""",
}
HARNESS = r"""
#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"
#include "fujinet/io/core/packet_link.h"
#include <algorithm>
#include <cassert>
#include <deque>
extern "C" {
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
}
using namespace fujinet::platform::esp32;
using namespace fujinet::io;
static std::deque<uint8_t> rx[2];
static bool ready = true;
static unsigned queries = 0;
namespace fujinet::platform::esp32 {
bool ensure_tinyusb_driver() { return ready; }
bool ensure_tinyusb_cdc_acm(UsbCdcAcmPort port) {
    assert(static_cast<int>(port) == CONFIG_FN_FUJIBUS_USB_CDC_PORT);
    return ready;
}
}
extern "C" uint32_t tud_cdc_n_available(uint8_t itf) {
    ++queries;
    return rx[itf].size();
}
extern "C" esp_err_t tinyusb_cdcacm_read(tinyusb_cdcacm_itf_t itf,
        uint8_t* out, size_t limit, size_t* count) {
    *count = std::min(limit, rx[itf].size());
    for (size_t i = 0; i < *count; ++i) { out[i] = rx[itf].front(); rx[itf].pop_front(); }
    return ESP_OK;
}
extern "C" size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t,
        const uint8_t*, size_t size) { return size; }
extern "C" esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t, uint32_t) { return ESP_OK; }
int main() {
    constexpr int data = CONFIG_FN_FUJIBUS_USB_CDC_PORT;
    UsbCdcChannel channel;
    PacketLink link(channel, 64, [] { return uint64_t{0}; });
    uint8_t buf[64];
    // Previously this never returned: driver ready was mistaken for RX data.
    assert(link.receive(buf, sizeof(buf)).status == PacketIOStatus::NoData);
    assert(!channel.available());
    // Console input must not make the packet channel readable.
    rx[1 - data] = {'h', 'e', 'l', 'p'};
    assert(!channel.available());
    rx[data] = {1, 2, 3};
    assert(channel.available());
    assert(channel.read(buf, 1) == 1 && buf[0] == 1);
    assert(channel.available());
    assert(channel.read(buf, sizeof(buf)) == 2 && buf[0] == 2 && buf[1] == 3);
    assert(!channel.available());
    // A partial packet must also yield while waiting for the rest.
    rx[data] = {0xF5, 0x4E};
    assert(link.receive(buf, sizeof(buf)).status == PacketIOStatus::Incomplete);
    assert(link.receive(buf, sizeof(buf)).status == PacketIOStatus::Incomplete);
    ready = false;
    const auto before = queries;
    assert(!channel.available());
    assert(queries == before);
}
"""


class UsbCdcChannelTest(unittest.TestCase):
    def test_readability_and_idle_packet_link(self):
        with tempfile.TemporaryDirectory(prefix="nio-usb-test-") as tmp:
            folder = Path(tmp)
            for name, body in HEADERS.items():
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            harness = folder / "test.cpp"
            harness.write_text(HARNESS)
            for port in (0, 1):
                with self.subTest(port=port):
                    binary = folder / f"test-{port}"
                    subprocess.run([
                        *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        f"-DCONFIG_FN_FUJIBUS_USB_CDC_PORT={port}",
                        "-I", str(folder), "-I", str(ROOT / "include"),
                        str(ROOT / "src/platform/esp32/usb_cdc_channel.cpp"),
                        str(ROOT / "src/lib/packet_link.cpp"), str(harness),
                        "-o", str(binary),
                    ], check=True, timeout=60)
                    subprocess.run([str(binary)], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
