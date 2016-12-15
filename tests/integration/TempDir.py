from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import os
import shutil
import sys
import tempfile
import time
import atexit
import path_utils as path

global_temp_dir = None


class TempDir(object):
    '''
    This is a helper for locating a reasonable place for temporary files.
    When run in the watchman test suite, we compute this up-front and then
    store everything under that temporary directory.
    When run under the FB internal test runner, we infer a reasonable grouped
    location from the process group environmental variable exported by the
    test runner.
    '''

    def __init__(self, keepAtShutdown=False):
        # We'll put all our temporary stuff under one dir so that we
        # can clean it all up at the end.

        # If we're running under an internal test runner, we prefer to put
        # things in a dir relating to that runner.
        parent_dir = tempfile.tempdir or os.environ.get('TMP', '/tmp')

        if 'TESTPILOT_PROCESS_GROUP' in os.environ:
            parent_dir = os.path.join(
                parent_dir,
                'watchmantest-%s' % (os.environ['TESTPILOT_PROCESS_GROUP'])
            )
            if not os.path.exists(parent_dir):
                os.mkdir(parent_dir)

            prefix = ''
        else:
            prefix = 'watchmantest'

        self.temp_dir = path.get_canonical_filesystem_path(
            tempfile.mkdtemp(dir=parent_dir, prefix=prefix))

        if os.name != 'nt':
            # On some platforms, setting the setgid bit on a directory doesn't
            # work if the user isn't a member of the directory's group. Set the
            # group explicitly to avoid this.
            os.chown(self.temp_dir, -1, os.getegid())
            # Some environments have a weird umask that can leave state
            # directories too open and break tests.
            os.umask(0o022)
        # Redirect all temporary files to that location
        tempfile.tempdir = self.temp_dir

        self.keep = keepAtShutdown

        def cleanup():
            if self.keep:
                sys.stdout.write('Preserving output in %s\n' % self.temp_dir)
                return
            self._retry_rmtree(self.temp_dir)

        atexit.register(cleanup)

    def get_dir(self):
        return self.temp_dir

    def set_keep(self, value):
        self.keep = value

    def _retry_rmtree(self, top):
        # Keep trying to remove it; on Windows it may take a few moments
        # for any outstanding locks/handles to be released
        for i in range(1, 10):
            shutil.rmtree(top, ignore_errors=True)
            if not os.path.isdir(top):
                return
            time.sleep(0.2)
        sys.stdout.write('Failed to completely remove ' + top)


def get_temp_dir(keep=None):
    global global_temp_dir
    if global_temp_dir:
        return global_temp_dir
    if keep is None:
        keep = os.environ.get('WATCHMAN_TEST_KEEP', '0') == '1'
    global_temp_dir = TempDir(keep)
    return global_temp_dir
