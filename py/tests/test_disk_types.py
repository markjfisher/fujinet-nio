import unittest

from fujinet_tools import disk
from fujinet_tools import diskproto as dp


class DiskImageTypeTests(unittest.TestCase):
    def test_every_type_round_trips_between_name_and_wire_value(self):
        for value, name in disk.TYPE_TEXT.items():
            self.assertEqual(disk._type_parse(name), value)
            self.assertEqual(disk._type_str(value), name)

    def test_diskcopy42_matches_cpp_image_type(self):
        # disk::ImageType::DiskCopy42 in include/fujinet/disk/disk_types.h
        self.assertEqual(dp.TYPE_DC42, 5)
        self.assertEqual(disk._type_parse("DC42"), dp.TYPE_DC42)

    def test_d64_matches_cpp_image_type(self):
        self.assertEqual(dp.TYPE_D64, 7)
        self.assertEqual(disk._type_parse("D64"), dp.TYPE_D64)
        self.assertEqual(disk._type_str(7), "d64")

    def test_prg_matches_cpp_image_type(self):
        self.assertEqual(dp.TYPE_PRG, 8)
        self.assertEqual(disk._type_parse("PRG"), dp.TYPE_PRG)
        self.assertEqual(disk._type_str(8), "prg")

    def test_xexboot_matches_cpp_image_type(self):
        # disk::ImageType::XexBoot in include/fujinet/disk/disk_types.h
        self.assertEqual(dp.TYPE_XEXBOOT, 6)
        self.assertEqual(disk._type_parse("XexBoot"), dp.TYPE_XEXBOOT)
        self.assertEqual(disk._type_str(6), "xexboot")

    def test_unknown_type_is_rejected(self):
        with self.assertRaises(ValueError):
            disk._type_parse("woz")


class DiskErrorTests(unittest.TestCase):
    def test_names_match_cpp_disk_error(self):
        # disk::DiskError in include/fujinet/disk/disk_types.h; wire values.
        self.assertEqual(dp.disk_error_name(0), "None")
        self.assertEqual(dp.disk_error_name(8), "BadImage")
        self.assertEqual(dp.disk_error_name(15), "GeometryRequired")
        self.assertEqual(dp.DISK_ERR_GEOMETRY_REQUIRED, 15)
        self.assertEqual(dp.disk_error_name(99), "Unknown(99)")

    def test_failure_payload_is_version_and_error(self):
        self.assertEqual(dp.parse_error_resp(bytes([1, 15])), 15)
        self.assertIsNone(dp.parse_error_resp(b""))
        self.assertIsNone(dp.parse_error_resp(bytes([2, 15])))   # other version
        self.assertIsNone(dp.parse_error_resp(bytes([1, 0, 0])))  # not a failure payload

    def test_failed_mount_reports_the_disk_error(self):
        class Pkt:
            payload = bytes([1, 15])
        text = disk._disk_error_str(Pkt())
        self.assertIn("GeometryRequired", text)
        self.assertIn("--sector-size", text)


if __name__ == "__main__":
    unittest.main()
