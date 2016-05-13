
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
