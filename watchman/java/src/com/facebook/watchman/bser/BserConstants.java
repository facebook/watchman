/*
 * Copyright 2015-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

package com.facebook.watchman.bser;

public class BserConstants {
  // Utility class, do not instantiate.
  private BserConstants() { }

  public static final byte BSER_ARRAY = 0x00;
  public static final byte BSER_OBJECT = 0x01;
  public static final byte BSER_STRING = 0x02;
  public static final byte BSER_INT8 = 0x03;
  public static final byte BSER_INT16 = 0x04;
  public static final byte BSER_INT32 = 0x05;
  public static final byte BSER_INT64 = 0x06;
  public static final byte BSER_REAL = 0x07;
  public static final byte BSER_TRUE = 0x08;
  public static final byte BSER_FALSE = 0x09;
  public static final byte BSER_NULL = 0x0a;
  public static final byte BSER_TEMPLATE = 0x0b;
  public static final byte BSER_SKIP = 0x0c;
}
