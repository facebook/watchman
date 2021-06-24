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

package com.facebook.watchman;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.Callable;
import java.util.concurrent.LinkedBlockingQueue;

import org.junit.Assert;
import org.junit.Before;

public class WatchmanTestBase {

  protected ByteArrayOutputStream mOutgoingMessageStream;
  protected BlockingQueue<Map<String, Object>> mObjectQueue;
  protected Callable<Map<String, Object>> mIncomingMessageGetter;

  @Before
  public void setUp() throws IOException {
    mOutgoingMessageStream = new ByteArrayOutputStream();
    mObjectQueue = new LinkedBlockingQueue<>();
    mIncomingMessageGetter = new Callable<Map<String, Object>>() {
      @Override
      public Map<String, Object> call() throws Exception {
        return mObjectQueue.take();
      }
    };
  }

  @SuppressWarnings("unchecked")
  protected static void deepObjectEquals(List<Object> expected, List<Object> object) {
    Assert.assertNotNull(object);
    Assert.assertEquals(expected.size(), object.size());
    Iterator<Object> iteratorExpected = expected.iterator();
    Iterator<Object> iteratorObject = object.iterator();
    while (iteratorExpected.hasNext()) {
      Object elementExpected = iteratorExpected.next();
      Object elementObject = iteratorObject.next();
      if (elementExpected instanceof List) {
        Assert.assertTrue(elementObject instanceof List);
        deepObjectEquals((List<Object>) elementExpected, (List<Object>) elementObject);
      } else if (elementExpected instanceof Map) {
        Assert.assertTrue(elementObject instanceof Map);
        deepObjectEquals((Map<String, Object>) elementExpected, (Map<String,Object>) elementObject);
      } else {
        Assert.assertEquals(elementExpected, elementObject);
      }
    }
  }

  @SuppressWarnings("unchecked")
  protected static void deepObjectEquals(Map<String, Object> expected, Map<String, Object> object) {
    Assert.assertNotNull(object);
    Assert.assertEquals(expected.size(), object.size());
    for (String key: expected.keySet()) {
      Object elementExpected = expected.get(key);
      Object elementObject = object.get(key);
      if (elementExpected instanceof List) {
        Assert.assertTrue(elementObject instanceof List);
        deepObjectEquals((List<Object>) elementExpected, (List<Object>) elementObject);
      } else if (elementExpected instanceof Map) {
        Assert.assertTrue(elementObject instanceof Map);
        deepObjectEquals((Map<String, Object>) elementExpected, (Map<String,Object>) elementObject);
      } else {
        Assert.assertEquals(elementExpected, elementObject);
      }
    }
  }

}
