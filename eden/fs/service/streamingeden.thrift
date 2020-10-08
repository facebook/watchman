/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

include "eden/fs/service/eden.thrift"
namespace cpp2 facebook.eden
namespace py3 eden.fs.service

/** This file holds definitions for the streaming flavor of the Eden interface
 * This is only available to cpp2 clients and won't compile for other
 * language/runtimes. */

service StreamingEdenService extends eden.EdenService {
  /**
   * Request notification about changes to the journal for
   * the specified mountPoint.
   *
   * Do not use the JournalPosition values in the stream. Instead,
   * call getFilesChangedSince or getCurrentJournalPosition which
   * will return up-to-date information and unblock future notifications
   * on this subscription. If the subscriber never calls getFilesChangedSince
   * or getCurrentJournalPosition in response to a notification on this
   * stream, future notifications may not arrive.
   *
   * This is an implementation of the subscribe API using the
   * new rsocket based streaming thrift protocol.
   * The name is temporary: we want to make some API changes
   * but want to start pushing out an implementation now because
   * we've seen inflated memory usage for the older `subscribe`
   * method above.
   */
  stream<eden.JournalPosition> subscribeStreamTemporary(
    1: eden.PathString mountPoint)
}
