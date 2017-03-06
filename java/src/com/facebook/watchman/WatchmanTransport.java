package com.facebook.watchman;

import java.io.Closeable;
import java.io.InputStream;
import java.io.OutputStream;

public interface WatchmanTransport extends Closeable {
    InputStream getInputStream();
    OutputStream getOutputStream();
}
