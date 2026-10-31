import unittest
from fujinet_tools.fuji import build_hsio_req, parse_hsio_resp


class FujiHsioTests(unittest.TestCase):
    def test_wire(self):
        self.assertEqual(build_hsio_req(), b'\x01')
        self.assertEqual(build_hsio_req(4), b'\x01\x04')
        self.assertEqual(build_hsio_req(255), b'\x01\xff')
        self.assertEqual(parse_hsio_resp(b'\x01\xff'), 255)
        self.assertEqual(parse_hsio_resp(b'\x01\x04'), 4)
        for bad in (b'', b'\x01', b'\x02\x04', b'\x01\x0a', b'\x01\x04\x00'):
            with self.assertRaises(ValueError): parse_hsio_resp(bad)
        with self.assertRaises(ValueError): build_hsio_req(10)
