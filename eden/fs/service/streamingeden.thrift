/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

include "eden/fs/service/eden.thrift"
namespace cpp2 facebook.eden
namespace py3 eden.fs.service

enum FsEventType {
  UNKNOWN = 0,
  START = 1,
  FINISH = 2,
}

struct FsEvent {
  // Nanoseconds since epoch.
  1: i64 timestamp;
  // Nanoseconds since arbitrary clock base, used for computing request
  // durations between start and finish.
  2: i64 monotonic_time_ns;

  3: FsEventType type;

  // See fuseRequest or prjfsRequest for the request opcode name.
  4: string arguments;

  // Always defined on Linux and macOS, but marked optional to support Windows.
  5: eden.FuseCall fuseRequest;
// To add Windows support, mark fuseRequest optional, and add:
// 6: optional eden.PrjfsCall prjfsRequest
}

/*
 * Bits that control the events traced from traceFsEvents.
 *
 * edenfs internally categorizes FUSE requests as read, write, or other. That
 * is subject to change, and additional filtering bits may be added in the
 * future.
 */

const i64 FS_EVENT_READ = 1
const i64 FS_EVENT_WRITE = 2
const i64 FS_EVENT_OTHER = 4

/**
 * This Thrift service defines streaming functions. It is separate from
 * EdenService because older Thrift runtimes do not support Thrift streaming,
 * primarily javadeprecated which is used by Buck. When Buck is updated to
 * use java-swift instead, we can merge EdenService and StreamingEdenService.
 */
service StreamingEdenService extends eden.EdenService {
  /**
   * Request notification about changes to the journal for
   * the specified mountPoint.
   *
   * IMPORTANT: Do not use the JournalPosition values in the stream. They are
   * meaningless. Instead, call getFilesChangedSince or
   * getCurrentJournalPosition which will return up-to-date information and
   * unblock future notifications on this subscription. If the subscriber
   * never calls getFilesChangedSince or getCurrentJournalPosition in
   * response to a notification on this stream, future notifications may not
   * arrive.
   *
   * This is an implementation of the subscribe API using the
   * new rsocket based streaming thrift protocol.
   * The name is temporary: we want to make some API changes
   * but want to start pushing out an implementation now because
   * we've seen inflated memory usage for the older `subscribe`
   * method above.
   */
  stream<eden.JournalPosition> subscribeStreamTemporary(
    1: eden.PathString mountPoint
  );

  /**
   * Returns, in order, a stream of FUSE or PrjFS requests and responses for
   * the given mount.
   *
   * eventCategoryMask is a bitset which indicates the requested trace events.
   * 0 indicates all events are requested.
   */
  stream<FsEvent> traceFsEvents(
    1: eden.PathString mountPoint,
    2: i64 eventCategoryMask);
}
