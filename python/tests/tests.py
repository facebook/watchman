#!/usr/bin/env python
# vim:ts=4:sw=4:et:
import unittest
import pywatchman
import os
from pywatchman import bser

class TestBSERDump(unittest.TestCase):
    def roundtrip(self, val):
        enc = bser.dumps(val)
        print "# %s  -->  %s" % (val, enc.encode('hex'))
        dec = bser.loads(enc)
        self.assertEquals(val, dec)

    def munged(self, val, munged):
        enc = bser.dumps(val)
        if isinstance(val, unicode):
            print "# %s  -->  %s" % (val.encode('utf-8'), enc.encode('hex'))
        else:
            print "# %s  -->  %s" % (val, enc.encode('hex'))
        dec = bser.loads(enc)
        self.assertEquals(munged, dec)

    def test_int(self):
        self.roundtrip(1)
        self.roundtrip(0x100)
        self.roundtrip(0x10000)
        self.roundtrip(0x10000000)
        self.roundtrip(0x1000000000)

    def test_float(self):
        self.roundtrip(1.5)

    def test_bool(self):
        self.roundtrip(True)
        self.roundtrip(False)

    def test_none(self):
        self.roundtrip(None)

    def test_string(self):
        self.roundtrip("hello")
        self.roundtrip(u'Hello')
        ustr = u'\xe4\xf6\xfc'
        self.munged(ustr, ustr.encode('utf-8'))

    def test_list(self):
        self.roundtrip([1, 2, 3])
        self.roundtrip([1, "helo", 2.5, False, None, True, 3])

    def test_tuple(self):
        self.munged((1, 2, 3), [1, 2, 3])

    def test_dict(self):
        self.roundtrip({"hello": "there"})

    def test_template(self):
        # since we can't generate the template bser output, here's a
        # a blob from the C test suite in watchman
        templ = "\x00\x01\x03\x28" + \
                "\x0b\x00\x03\x02\x02\x03\x04\x6e\x61\x6d\x65\x02" + \
                "\x03\x03\x61\x67\x65\x03\x03\x02\x03\x04\x66\x72" + \
                "\x65\x64\x03\x14\x02\x03\x04\x70\x65\x74\x65\x03" + \
                "\x1e\x0c\x03\x19"
        dec = bser.loads(templ)
        exp = [
            {"name": "fred", "age": 20},
            {"name": "pete", "age": 30},
            {"age": 25}
        ]
        self.assertEquals(exp, dec)

    def test_pdu_len(self):
        enc = bser.dumps(1)
        self.assertEquals(len(enc), bser.pdu_len(enc))

        # try a bigger one; prove that we get the correct length
        # even though we receive just a portion of the complete
        # data
        enc = bser.dumps([1, 2, 3, "hello there, much larger"])
        self.assertEquals(len(enc), bser.pdu_len(enc[0:7]))

    def test_client(self):
        # verify that we can talk to the instance set up by the harness
        # we're just verifying that the socket set in the environment works
        # and that we can understand the result
        sock = os.getenv('WATCHMAN_SOCK')
        c = pywatchman.client()
        res = c.query('get-sockname')
        self.assertEquals(sock, res['sockname'])

if __name__ == '__main__':
    unittest.main()

