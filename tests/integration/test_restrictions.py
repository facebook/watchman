# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import json
import os
import tempfile
import pywatchman

import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchRestrictions(WatchmanTestCase.WatchmanTestCase):
    def test_rootRestrict(self):
      config = {
          'root_restrict_files': ['.git', '.hg', '.foo', '.bar']
      }

      inst = WatchmanInstance.Instance(config=config)
      try:
          inst.start()
          client = self.getClient(inst)

          expect = [
              ('directory', '.git', True),
              ('directory', '.hg', True),
              ('file', '.foo', True),
              ('file', '.bar', True),
              ('directory', '.bar', True),
              (None, None, False),
              ('directory', '.svn', False),
              ('file', 'baz', False),
          ]

          for filetype, name, expect_pass in expect:
              # encode the test criteria in the dirname so that we can
              # figure out which test scenario failed more easily
              d = self.mkdtemp(suffix='-%s-%s-%s' % (
                               filetype, name, expect_pass))
              if filetype == 'directory':
                  os.mkdir(os.path.join(d, name))
              elif filetype == 'file':
                  self.touchRelative(d, name)

              if expect_pass:
                  client.query('watch', d)
              else:
                  with self.assertRaises(pywatchman.WatchmanError) as ctx:
                      client.query('watch', d)
                  self.assertRegexpMatches(
                      str(ctx.exception),
                      ("unable to resolve root .*"+
                      ": Your watchman administrator has configured watchman "+
                      "to prevent watching this path.  "+
                      "None of the files listed in global config root_files "+
                      "are present and enforce_root_files is set to true"))

      finally:
          inst.stop()

    def test_invalidRoot(self):
        d = self.mkdtemp()
        invalid = os.path.join(d, "invalid")
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('watch', invalid)
        msg = str(ctx.exception)
        if 'No such file or directory' in msg:
            # unix
            return
        if 'The system cannot find the file specified' in msg:
            # windows
            return
        self.assertTrue(False, msg)
