#!/usr/bin/env python
# vim:ts=4:sw=4:et:
import inspect
import unittest
import os
from pywatchman import bser, pybser, SocketTimeout, WatchmanError


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

    def roundtrip(self, val):
        enc = self.bser_mod.dumps(val)
        print "# %s  -->  %s" % (val, enc.encode('hex'))
        dec = self.bser_mod.loads(enc)
        self.assertEquals(val, dec)

    def munged(self, val, munged):
        enc = self.bser_mod.dumps(val)
        if isinstance(val, unicode):
            print "# %s  -->  %s" % (val.encode('utf-8'), enc.encode('hex'))
        else:
            print "# %s  -->  %s" % (val, enc.encode('hex'))
        dec = self.bser_mod.loads(enc)
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
        dec = self.bser_mod.loads(templ)
        exp = [
            {"name": "fred", "age": 20},
            {"name": "pete", "age": 30},
            {"age": 25}
        ]
        self.assertEquals(exp, dec)

    def test_pdu_len(self):
        enc = self.bser_mod.dumps(1)
        self.assertEquals(len(enc), self.bser_mod.pdu_len(enc))

        # try a bigger one; prove that we get the correct length
        # even though we receive just a portion of the complete
        # data
        enc = self.bser_mod.dumps([1, 2, 3, "hello there, much larger"])
        self.assertEquals(len(enc), self.bser_mod.pdu_len(enc[0:7]))

def load_tests(loader, test_methods=None, pattern=None):
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(TestSocketTimeout))
    for bser_mod in (bser, pybser):
        suite.addTest(TestBSERDump.parameterize(loader, bser_mod))
    return suite

if __name__ == '__main__':
    suite = load_tests(unittest.TestLoader())
    unittest.TextTestRunner().run(suite)
