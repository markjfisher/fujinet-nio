from __future__ import annotations

import configparser
import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORM_DIR = REPO_ROOT / "pio-build" / "ini" / "platforms"
SDKCONFIG_MAP = REPO_ROOT / "pio-build" / "sdkconfig" / "00_platform_sdkconfig_map.txt"
SDKCONFIG_DIR = REPO_ROOT / "pio-build" / "sdkconfig"
BOARDS_DIR = REPO_ROOT / "boards"
ATARI_PROFILES = tuple(
    f"atari-{kind}-sio-gpio-fujinet-v1-{size}mb"
    for size in (8, 16)
    for kind in ("legacy", "nio-fujibus")
)


def _read_ini(path: Path) -> configparser.RawConfigParser:
    config = configparser.RawConfigParser()
    read = config.read(path)
    if not read:
        raise AssertionError(f"failed to read ini file: {path}")
    return config


def _platform_fragments() -> list[Path]:
    return sorted(PLATFORM_DIR.glob("platformio-*.ini"))


def _board_name_from_fragment(path: Path) -> str:
    return path.stem.removeprefix("platformio-")


def _read_sdkconfig_map() -> dict[str, list[str]]:
    entries: dict[str, list[str]] = {}
    for raw_line in SDKCONFIG_MAP.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep:
            continue
        entries[key.strip()] = [item.strip() for item in value.split(",") if item.strip()]
    return entries


class TestPlatformIOBoardConfig(unittest.TestCase):
    def test_atari_psram_defaults_match_original_esp32_hardware(self) -> None:
        sdkconfig_map = _read_sdkconfig_map()
        for name in ATARI_PROFILES:
            with self.subTest(board_name=name):
                config = _read_ini(PLATFORM_DIR / f"platformio-{name}.ini")
                board_id = config.get(f"env:{name}", "board")
                board = json.loads((BOARDS_DIR / f"{board_id}.json").read_text())
                defaults = {}
                for template in sdkconfig_map[name]:
                    for line in (SDKCONFIG_DIR / f"{template}.defaults").read_text().splitlines():
                        key, sep, value = line.partition("=")
                        if sep and key.startswith("CONFIG_"):
                            defaults[key] = value.strip('"')

                self.assertEqual(board["build"]["mcu"], "esp32")
                self.assertEqual(board["build"]["f_flash"], "40000000L")
                self.assertEqual(defaults["CONFIG_SPIRAM"], "y")
                self.assertEqual(defaults["CONFIG_SPIRAM_MODE_QUAD"], "y")
                self.assertEqual(defaults["CONFIG_SPIRAM_SPEED_40M"], "y")
                self.assertEqual(defaults["CONFIG_SPIRAM_USE_MALLOC"], "y")
                self.assertEqual(defaults["CONFIG_SPIRAM_CACHE_WORKAROUND"], "y")
                self.assertEqual(defaults["CONFIG_ESP_WIFI_IRAM_OPT"], "n")
                self.assertEqual(defaults["CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM"], "n")
                self.assertEqual(defaults["CONFIG_FATFS_ALLOC_PREFER_EXTRAM"], "y")
                self.assertNotIn("CONFIG_SPIRAM_MODE_OCT", defaults)
                self.assertNotIn("CONFIG_SPIRAM_CS_IO", defaults)

    def test_atari_flash_defaults_match_board_and_partition_layout(self) -> None:
        sdkconfig_map = _read_sdkconfig_map()
        for name in ATARI_PROFILES:
            with self.subTest(board_name=name):
                config = _read_ini(PLATFORM_DIR / f"platformio-{name}.ini")
                board_id = config.get(f"env:{name}", "board")
                board = json.loads((BOARDS_DIR / f"{board_id}.json").read_text())
                defaults = {}
                for template in sdkconfig_map[name]:
                    for line in (SDKCONFIG_DIR / f"{template}.defaults").read_text().splitlines():
                        key, sep, value = line.partition("=")
                        if sep and key.startswith("CONFIG_"):
                            defaults[key] = value.strip('"')

                flash_mb = 16 if name.endswith("16mb") else 8
                self.assertEqual(board["upload"]["flash_size"], f"{flash_mb}MB")
                self.assertEqual(board["upload"]["maximum_size"], flash_mb * 1024 * 1024)
                self.assertEqual(defaults[f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash_mb}MB"], "y")
                self.assertEqual(defaults["CONFIG_PARTITION_TABLE_CUSTOM"], "y")
                partitions = defaults["CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"]
                self.assertEqual(partitions, board["build"]["partitions"])
                self.assertTrue((REPO_ROOT / partitions).is_file())
                end = 0
                for row in (REPO_ROOT / partitions).read_text().splitlines():
                    if not row.strip() or row.lstrip().startswith("#"):
                        continue
                    fields = [field.strip() for field in row.split(",")]
                    offset, size = int(fields[3], 0), int(fields[4], 0)
                    self.assertGreaterEqual(offset, end)
                    end = offset + size
                self.assertEqual(end, flash_mb * 1024 * 1024)

    def test_all_platform_fragments_are_selectable(self) -> None:
        sdkconfig_map = _read_sdkconfig_map()
        fragments = _platform_fragments()
        self.assertGreater(len(fragments), 0)

        for fragment in fragments:
            with self.subTest(fragment=fragment.name):
                name = _board_name_from_fragment(fragment)
                config = _read_ini(fragment)

                self.assertEqual(config.get("fujinet", "build_board"), name)
                self.assertTrue(config.has_section(f"env:{name}"))
                self.assertIn(name, sdkconfig_map)

    def test_all_platform_fragments_reference_known_board_json(self) -> None:
        for fragment in _platform_fragments():
            with self.subTest(fragment=fragment.name):
                name = _board_name_from_fragment(fragment)
                config = _read_ini(fragment)
                board_id = config.get(f"env:{name}", "board")

                self.assertTrue(
                    (BOARDS_DIR / f"{board_id}.json").is_file(),
                    f"{fragment.name} references missing board JSON {board_id}.json",
                )

    def test_sdkconfig_map_entries_reference_existing_defaults(self) -> None:
        sdkconfig_map = _read_sdkconfig_map()
        for board_name, defaults in sdkconfig_map.items():
            with self.subTest(board_name=board_name):
                self.assertGreater(len(defaults), 0)
                for default_name in defaults:
                    self.assertTrue(
                        (SDKCONFIG_DIR / f"{default_name}.defaults").is_file(),
                        f"{board_name} references missing sdkconfig default {default_name}.defaults",
                    )

    def test_atari_legacy_and_nio_board_names_select_different_profiles(self) -> None:
        for size in (8, 16):
            with self.subTest(flash_mb=size):
                legacy_name = f"atari-legacy-sio-gpio-fujinet-v1-{size}mb"
                nio_name = f"atari-nio-fujibus-sio-gpio-fujinet-v1-{size}mb"
                legacy = _read_ini(PLATFORM_DIR / f"platformio-{legacy_name}.ini")
                nio = _read_ini(PLATFORM_DIR / f"platformio-{nio_name}.ini")
                legacy_flags = legacy.get(f"env:{legacy_name}", "build_flags")
                nio_flags = nio.get(f"env:{nio_name}", "build_flags")

                self.assertIn("-DFN_BUILD_ATARI_SIO", legacy_flags)
                self.assertNotIn("-DFN_BUILD_ATARI_FUJIBUS_SIO", legacy_flags)
                self.assertIn("-DFN_BUILD_ATARI_FUJIBUS_SIO", nio_flags)
                self.assertNotIn("-DFN_BUILD_ATARI_SIO", nio_flags)

    def test_atari_nio_v1_board_targets_old_esp32_sio_hardware(self) -> None:
        for size in (8, 16):
            with self.subTest(flash_mb=size):
                name = f"atari-nio-fujibus-sio-gpio-fujinet-v1-{size}mb"
                config = _read_ini(PLATFORM_DIR / f"platformio-{name}.ini")
                section = f"env:{name}"

                self.assertEqual(config.get(section, "board"), f"fujinet-v1-{size}mb")
                self.assertEqual(
                    config.get(section, "board_build.cmake_extra_args"),
                    "-D CONFIG_IDF_TARGET_ESP32=1",
                )

                flags = config.get(section, "build_flags")
                self.assertIn("-DFN_BUILD_ATARI_FUJIBUS_SIO", flags)
                self.assertIn("-DFN_PINMAP_DEFAULT=FN_PINMAP_ATARIV1", flags)

                board = (BOARDS_DIR / f"fujinet-v1-{size}mb.json").read_text()
                self.assertIn('"mcu": "esp32"', board)
                self.assertIn('"maximum_ram_size": 327680', board)

                sdkconfig_map = _read_sdkconfig_map()
                self.assertIn("fujibus-gpio", sdkconfig_map[name])
                self.assertNotIn("spiram-oct80", sdkconfig_map[name])
