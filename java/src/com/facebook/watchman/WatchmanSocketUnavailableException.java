package com.facebook.watchman;

/**
 * Created by cotizo on 5/31/16.
 */
public class WatchmanSocketUnavailableException extends Exception {

  public WatchmanSocketUnavailableException() {
    super();
  }

  public WatchmanSocketUnavailableException(String message) {
    super(message);
  }

  public WatchmanSocketUnavailableException(String message, Throwable cause) {
    super(message, cause);
  }
}
