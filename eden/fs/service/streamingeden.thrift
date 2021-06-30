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

/**
 * Common timestamps for every trace event, used to measure durations and
 * display wall clock time.
 */
struct TraceEventTimes {
  // Nanoseconds since epoch.
  1: i64 timestamp;
  // Nanoseconds since arbitrary clock base, used for computing request
  // durations between start and finish.
  2: i64 monotonic_time_ns;
}

struct RequestInfo {
  // The pid that originated this request.
  1: optional eden.pid_t pid;
  // If available, the binary name corresponding to `pid`.
  2: optional string processName;
}

struct FsEvent {
  // Nanoseconds since epoch.
  1: i64 timestamp;
  // Nanoseconds since arbitrary clock base, used for computing request
  // durations between start and finish.
  2: i64 monotonic_time_ns;

  7: TraceEventTimes times;

  3: FsEventType type;

  // See fuseRequest or prjfsRequest for the request opcode name.
  4: string arguments;

  // At most one of the *Request fields will be set, depending on the filesystem implementation.
  5: optional eden.FuseCall fuseRequest;
  10: optional eden.NfsCall nfsRequest;
  // To add Windows support:
  // 6: optional eden.PrjfsCall prjfsRequest;

  8: RequestInfo requestInfo;

  /**
   * The result code sent back to the kernel.
   *
   * Positive is success, and, depending on the operation, may contain a nonzero result.
   *
   * If a FUSE request returns an inode which the kernel will reference, this field contains that inode numebr, so it can be correlated with future FUSE requests to that inode.
   * field is set. This field can be used to link the lookup/create/mknod
   * request to future FUSE requests on that inode.
   *
   * Negative indicates an error.
   */
  9: optional i64 result;
}

/*
 * Bits that control the events traced from traceFsEvents.
 *
 * edenfs internally categorizes FUSE requests as read, write, or other. That
 * is subject to change, and additional filtering bits may be added in the
 * future.
 */

const i64 FS_EVENT_READ = 1;
const i64 FS_EVENT_WRITE = 2;
const i64 FS_EVENT_OTHER = 4;

enum HgEventType {
  UNKNOWN = 0,
  QUEUE = 1,
  START = 2,
  FINISH = 3,
}

enum HgResourceType {
  UNKNOWN = 0,
  BLOB = 1,
  TREE = 2,
}

struct HgEvent {
  1: TraceEventTimes times;

  2: HgEventType eventType;
  3: HgResourceType resourceType;

  4: i64 unique;

  // HG manifest node ID as 40-character hex string.
  5: string manifestNodeId;
  6: binary path;

  7: optional RequestInfo requestInfo;
}

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
    1: eden.PathString mountPoint,
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
    2: i64 eventCategoryMask,
  );

  /**
   * Returns, in order, a stream of hg import requests for the given mount.
   *
   * Each request has a unique ID and transitions through three states: queued,
   * started, and finished.
   */
  stream<HgEvent> traceHgEvents(1: eden.PathString mountPoint);
}
