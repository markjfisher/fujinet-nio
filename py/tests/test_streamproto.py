import doctest
import unittest
from fujinet_tools import streamproto as s

class StreamWireTests(unittest.TestCase):
    def test_independent_vectors(self):
        self.assertEqual(s.request(0x78563412, 0x44332211, 2, length=7),
                         bytes.fromhex('0102123456781122334407'))
        self.assertEqual(s.request(1, 2, 3, b'ab'), bytes.fromhex('01030100000002000000026162'))
        self.assertEqual(s.response(bytes.fromhex('00040101000000020000005a0000'),1,2,2,7),
                         dict(status=0, flags=4, actual=1, data='5a'))
        self.assertEqual(s.response(bytes.fromhex('0000010100000002000000'),1,2,3,2)['actual'],1)
    def test_bounds(self):
        for args in [(0,0,2,b'',1),(1,0,2,b'',0),(1,0,2,b'',121),
                     (1,0,3,b''),(1,0,1,b'http://x'),(1,0,4,b'',1)]:
            with self.assertRaises(ValueError): s.request(*args)
        for data in ['00','00000102000000020000005a','00000201000000020000005a',
                     '00000801000000020000005a','04000101000000020000005a']:
            with self.assertRaises(ValueError): s.response(bytes.fromhex(data),1,2,2,7)
    def test_doctests(self):
        self.assertEqual(doctest.testmod(s).failed,0)
