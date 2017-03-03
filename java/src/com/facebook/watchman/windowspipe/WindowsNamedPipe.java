package com.facebook.watchman.windowspipe;


import com.facebook.watchman.WatchmanTransport;
import com.google.common.base.Preconditions;
import com.sun.jna.Memory;
import com.sun.jna.platform.win32.WinBase;
import com.sun.jna.platform.win32.WinError;
import com.sun.jna.platform.win32.WinNT;
import com.sun.jna.ptr.IntByReference;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;

public class WindowsNamedPipe implements WatchmanTransport {
    private final WinNT.HANDLE pipeHandle;
    private final InputStream in;
    private final OutputStream out;
    private final WinNT.HANDLE readerWaitable;
    private final WinNT.HANDLE writerWaitable;


    public WindowsNamedPipe(String path) throws IOException {

        this.pipeHandle =
                WindowsNamedPipeLibrary.INSTANCE.CreateFile(
                        path,
                        WinNT.GENERIC_READ | WinNT.GENERIC_WRITE,
                        0,
                        null,
                        WinNT.OPEN_EXISTING,
                        WinNT.FILE_FLAG_OVERLAPPED,
                        null
                );
        Preconditions.checkState(
                !WinNT.INVALID_HANDLE_VALUE.equals(pipeHandle),
                "CreateFile() failed to create a named pipe for %s",
                path);

        this.in = new NamedPipeInputStream();
        this.out = new NamedPipeOutputStream();

        this.readerWaitable = WindowsNamedPipeLibrary.INSTANCE.CreateEvent(null, true, false, null);
        this.writerWaitable = WindowsNamedPipeLibrary.INSTANCE.CreateEvent(null, true, false, null);
        Preconditions.checkState(readerWaitable != null);
        Preconditions.checkState(writerWaitable != null);
    }


    @Override
    public void close() throws IOException {
        Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.CloseHandle(pipeHandle));
        Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.CloseHandle(readerWaitable));
        Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.CloseHandle(writerWaitable));
    }

    @Override
    public InputStream getInputStream() {
        return in;
    }

    @Override
    public OutputStream getOutputStream() {
        return out;
    }

    private class NamedPipeOutputStream extends OutputStream {
        @Override
        public void write(int b) throws IOException {
            throw new IllegalStateException("NOT supported yet");
        }

        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            byte[] data = Arrays.copyOfRange(b, off, off + len);

            WinBase.OVERLAPPED olap = new WinBase.OVERLAPPED();
            olap.hEvent = writerWaitable;
            olap.write();

            boolean immediate = WindowsNamedPipeLibrary.INSTANCE.WriteFile(pipeHandle, data, len, null, olap.getPointer());
            if (!immediate) {
                Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.GetLastError() == WinError.ERROR_IO_PENDING);
            }
            IntByReference written = new IntByReference();
            Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.GetOverlappedResult(pipeHandle, olap.getPointer(), written, true));
            Preconditions.checkState(written.getValue() == len);
        }
    }

    private class NamedPipeInputStream extends InputStream {

        @Override
        public int read() throws IOException {
            throw new IllegalStateException("NOT supported yet");
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            Memory readBuffer = new Memory(len);

            WinBase.OVERLAPPED olap = new WinBase.OVERLAPPED();
            olap.hEvent = readerWaitable;
            olap.write();

            boolean immediate = WindowsNamedPipeLibrary.INSTANCE.ReadFile(pipeHandle, readBuffer, len, null, olap.getPointer());
            if (!immediate) {
                Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.GetLastError() == WinError.ERROR_IO_PENDING);
            }
            IntByReference read = new IntByReference();
            Preconditions.checkState(WindowsNamedPipeLibrary.INSTANCE.GetOverlappedResult(pipeHandle, olap.getPointer(), read, true));
            byte[] byteArray = readBuffer.getByteArray(0, len);
            Preconditions.checkState(read.getValue() == len);
            System.arraycopy(byteArray, 0, b, off, len);
            return len;
        }
    }

}