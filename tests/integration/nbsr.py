from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import threading

try:
    from queue import Queue, Empty
except BaseException:
    from Queue import Queue, Empty


# Non-blocking stream reader inspired by:
# https://stackoverflow.com/q/41431882/149111
class NonBlockingStreamReader:
    def __init__(self, stream):
        self.stream = stream
        self.queue = Queue()
        self.stop_event = threading.Event()

        def populateQueue(stream, queue):
            while not self.stop_event.is_set():
                try:
                    line = stream.readline()
                    if line:
                        queue.put(line)
                        continue
                except IOError:
                    pass
                if not self.stop_event.is_set():
                    raise EndOfStreamError
                break

        self.thread = threading.Thread(
            target=populateQueue, args=(self.stream, self.queue)
        )
        self.thread.daemon = True
        self.thread.start()

    def shutdown(self):
        self.stop_event.set()

    def stop(self, timeout=5):
        self.thread.join(timeout=timeout)

    def readline(self, timeout=None):
        try:
            return self.queue.get(block=timeout is not None, timeout=timeout)
        except Empty:
            return None


class EndOfStreamError(Exception):
    pass
