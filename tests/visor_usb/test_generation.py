"""Build ownership tests; no SDK, libusb, USB device or source mutation."""
import importlib.util
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('generator', ROOT / 'scripts/update_cmake_sources.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

class SourceOwnership(unittest.TestCase):
    def test_optional_sources_not_in_default_or_esp_lists(self):
        sources = generator.collect_cpp_files()
        for block in (generator.generate_posix_block(sources), generator.generate_esp32_block(sources)):
            self.assertNotIn('visor_usb/', block)
            self.assertNotIn('build_profile/visor_usb.cpp', block)
        self.assertNotIn('src/lib/build_profile/', generator.generate_posix_block(sources))

    def test_existing_profiles_and_template_are_preserved(self):
        template = (ROOT / 'build-artifacts/CMakeLists_posix.cmake').read_text()
        for profile in ('fujibus_link_tcp', 'fujibus_link_serial', 'fujibus_pty', 'atari_fujibus_netsio'):
            self.assertIn('src/lib/build_profile/' + profile + '.cpp', template)
        self.assertIn('if(FN_BUILD_VISOR_USB_HOST)', template)
        self.assertIn('pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.25)', template)
        self.assertIn('FN_WITH_HOTSYNC OR', template)

if __name__ == '__main__':
    unittest.main()
