#!/usr/bin/env python
# vim:ts=4:sw=4:et:
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import binascii
import inspect
import unittest
import os
import sys

from pywatchman import (
    bser,
    compat,
    pybser,
    SocketTimeout,
    WatchmanError,
)

PILE_OF_POO = u"\U0001F4A9"
NON_UTF8_STRING = b'\xff\xff\xff'

class TestSocketTimeout(unittest.TestCase):
    def test_exception_handling(self):
        try:
            raise SocketTimeout('should not raise')
        except WatchmanError:
            pass


class TestBSERDump(unittest.TestCase):
    # bser_mod will be None during discovery
    def __init__(self, method_name, bser_mod=None):
        super(TestBSERDump, self).__init__(method_name)
        if bser_mod:
            self._test_name = '%s.%s [%s]' % (
                self.__class__.__name__, method_name, bser_mod.__name__)
        else:
            self._test_name = None
        self.bser_mod = bser_mod

    @staticmethod
    def parameterize(loader, bser_mod):
        suite = unittest.TestSuite()
        for method_name in loader.getTestCaseNames(TestBSERDump):
            suite.addTest(TestBSERDump(method_name, bser_mod))
        return suite

    def id(self):
        if self._test_name:
            return self._test_name
        else:
            return super(TestBSERDump, self).id()

    def roundtrip(self, val, mutable=True, value_encoding=None,
                  value_errors=None):
        enc = self.bser_mod.dumps(val)
        print("# %s  -->  %s" % (repr(val),
                                 binascii.hexlify(enc).decode('ascii')))
        dec = self.bser_mod.loads(enc, mutable, value_encoding=value_encoding,
                                  value_errors=value_errors)
        self.assertEqual(val, dec)

    def munged(self, val, munged, value_encoding=None, value_errors=None):
        enc = self.bser_mod.dumps(val)
        print("# %s  -->  %s" % (repr(val),
                                 binascii.hexlify(enc).decode('ascii')))
        dec = self.bser_mod.loads(enc, value_encoding=value_encoding,
                                  value_errors=value_errors)
        self.assertEqual(munged, dec)

    def test_int(self):
        self.roundtrip(1)
        self.roundtrip(0x100)
        self.roundtrip(0x10000)
        self.roundtrip(0x10000000)
        self.roundtrip(0x1000000000)

    def test_negative_int(self):
        self.roundtrip(-0x80)
        self.roundtrip(-0x8000)
        self.roundtrip(-0x80000000)
        self.roundtrip(-0x8000000000000000)

    def test_float(self):
        self.roundtrip(1.5)

    def test_bool(self):
        self.roundtrip(True)
        self.roundtrip(False)

    def test_none(self):
        self.roundtrip(None)

    def test_string(self):
        self.roundtrip(b"hello")

        # For Python 3, here we can only check that a Unicode string goes in,
        # not that a Unicode string comes out.
        self.munged(u'Hello', b'Hello')

        self.roundtrip(u'Hello', value_encoding='utf8')
        self.roundtrip(u'Hello', value_encoding='ascii')
        self.roundtrip(u'Hello' + PILE_OF_POO, value_encoding='utf8')

        # can't use the with form here because Python 2.6
        self.assertRaises(UnicodeDecodeError, self.roundtrip,
                          u'Hello' + PILE_OF_POO, value_encoding='ascii')
        self.munged(u'Hello' + PILE_OF_POO, u'Hello', value_encoding='ascii',
                    value_errors='ignore')
        self.roundtrip(b'hello' + NON_UTF8_STRING)
        self.assertRaises(UnicodeDecodeError, self.roundtrip,
                          b'hello' + NON_UTF8_STRING, value_encoding='utf8')
        self.munged(b'hello' + NON_UTF8_STRING, u'hello', value_encoding='utf8',
                    value_errors='ignore')
        # TODO: test non-UTF8 strings with surrogateescape in Python 3

        ustr = u'\xe4\xf6\xfc'
        self.munged(ustr, ustr.encode('utf-8'))

    def test_list(self):
        self.roundtrip([1, 2, 3])
        self.roundtrip([1, b"helo", 2.5, False, None, True, 3])

    def test_tuple(self):
        self.munged((1, 2, 3), [1, 2, 3])
        self.roundtrip((1, 2, 3), mutable=False)

    def test_dict(self):
        self.roundtrip({"hello": b"there"})
        self.roundtrip({"hello": u"there"}, value_encoding='utf8')
        self.roundtrip({"hello": u"there"}, value_encoding='ascii')
        self.roundtrip({"hello": u"there" + PILE_OF_POO},
                       value_encoding='utf8')

        # can't use the with form here because Python 2.6
        self.assertRaises(UnicodeDecodeError, self.roundtrip,
                          {"hello": u"there" + PILE_OF_POO},
                          value_encoding='ascii')
        self.munged({'Hello': u'there' + PILE_OF_POO},
                    {'Hello': u'there'}, value_encoding='ascii',
                    value_errors='ignore')
        self.roundtrip({'Hello': b'there' + NON_UTF8_STRING})
        self.assertRaises(UnicodeDecodeError, self.roundtrip,
                          {"hello": b"there" + NON_UTF8_STRING},
                          value_encoding='utf8')
        self.munged({'Hello': b'there' + NON_UTF8_STRING},
                    {'Hello': u'there'}, value_encoding='utf8',
                    value_errors='ignore')

        obj = self.bser_mod.loads(self.bser_mod.dumps({"hello": b"there"}),
                                  False)
        self.assertEqual(1, len(obj))
        self.assertEqual(b'there', obj.hello)
        self.assertEqual(b'there', obj[u'hello'])
        if not compat.PYTHON3:
            self.assertEqual(b'there', obj[b'hello'])
        self.assertEqual(b'there', obj[0])
        # make sure this doesn't crash
        self.assertRaises(Exception, lambda: obj[45.25])

        hello, = obj  # sequence/list assignment
        self.assertEqual(b'there', hello)

    def assertItemAttributes(self, dictish, attrish):
        self.assertEqual(len(dictish), len(attrish))
        # Use items for compatibility across Python 2 and 3.
        for k, v in dictish.items():
            self.assertEqual(v, getattr(attrish, k))

    def test_template(self):
        # since we can't generate the template bser output, here's a
        # a blob from the C test suite in watchman
        templ = b"\x00\x01\x03\x28" + \
                b"\x0b\x00\x03\x02\x02\x03\x04\x6e\x61\x6d\x65\x02" + \
                b"\x03\x03\x61\x67\x65\x03\x03\x02\x03\x04\x66\x72" + \
                b"\x65\x64\x03\x14\x02\x03\x04\x70\x65\x74\x65\x03" + \
                b"\x1e\x0c\x03\x19"
        dec = self.bser_mod.loads(templ)
        exp = [
            {"name": b"fred", "age": 20},
            {"name": b"pete", "age": 30},
            {"name": None, "age": 25}
        ]
        self.assertEqual(exp, dec)
        res = self.bser_mod.loads(templ, False)

        for i in range(0, len(exp)):
            self.assertItemAttributes(exp[i], res[i])

    def test_pdu_len(self):
        enc = self.bser_mod.dumps(1)
        self.assertEqual(len(enc), self.bser_mod.pdu_len(enc))

        # try a bigger one; prove that we get the correct length
        # even though we receive just a portion of the complete
        # data
        enc = self.bser_mod.dumps([1, 2, 3, "hello there, much larger"])
        self.assertEqual(len(enc), self.bser_mod.pdu_len(enc[0:7]))

    def test_garbage(self):
        # can't use the with form here because Python 2.6
        self.assertRaises(ValueError, self.bser_mod.loads, b"\x00\x01\n")
        self.assertRaises(ValueError, self.bser_mod.loads,
                          b'\x00\x01\x04\x01\x00\x02')
        self.assertRaises(ValueError, self.bser_mod.loads, b'\x00\x01\x07')
        self.assertRaises(ValueError, self.bser_mod.loads,
                          b'\x00\x01\x03\x01\xff')

        self.assertRaises(ValueError, self.bser_mod.pdu_len, b'\x00\x02')

def load_tests(loader, test_methods=None, pattern=None):
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(TestSocketTimeout))
    for bser_mod in (bser, pybser):
        suite.addTest(TestBSERDump.parameterize(loader, bser_mod))
    return suite

if __name__ == '__main__':
    suite = load_tests(unittest.TestLoader())
    unittest.TextTestRunner().run(suite)
