"""The selected ESP32 USB profiles must keep logs off the FujiBus stream."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "pio-build" / "sdkconfig"
PROFILES = (
    "fujibus-usbcdc-nosd-s3-wroom-1-n16r8",
    "fujibus-usbcdc-freenove-s3-wroom-1-n16r8",
    "fujibus-link-usbcdc-freenove-s3-wroom-1-n16r8",
)


class UsbCdcProfilesTest(unittest.TestCase):
    def test_profile_fragments_separate_fujibus_and_logs(self):
        mappings = dict(line.split("=", 1) for line in
                        (SDK / "00_platform_sdkconfig_map.txt").read_text().splitlines()
                        if line and not line.startswith("#"))
        for profile in PROFILES:
            with self.subTest(profile=profile):
                config = {}
                for fragment in mappings[profile].split(","):
                    for line in (SDK / (fragment + ".defaults")).read_text().splitlines():
                        if line.startswith("CONFIG_") and "=" in line:
                            key, value = line.split("=", 1)
                            config[key] = value
                self.assertEqual(config["CONFIG_TINYUSB_CDC_COUNT"], "2")
                self.assertEqual(config["CONFIG_FN_FUJIBUS_USB_CDC_PORT"], "0")
                self.assertEqual(config["CONFIG_FN_ESP_CONSOLE_CDC_NUM"], "1")
