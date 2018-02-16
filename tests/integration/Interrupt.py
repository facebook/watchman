
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

interrupted = False


def wasInterrupted():
    global interrupted
    return interrupted


def setInterrupted():
    global interrupted
    interrupted = True


def checkInterrupt():
    '''
    If an interrupt was detected, raise it now.
    We use this to defer interrupt processing until we're
    in the right place to handle it.
    '''
    if wasInterrupted():
        raise KeyboardInterrupt()
