
interrupted = False

def wasInterrupted():
    global interrupted
    return interrupted

def setInterrupted():
    global interrupted
    interrupted = True
