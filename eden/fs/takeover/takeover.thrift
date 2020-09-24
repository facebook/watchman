/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

namespace cpp2 facebook.eden

// A list of takeover data serialization versions that the client supports
struct TakeoverVersionQuery {
  // The set of versions supported by the client
  1: set<i32> versions,
}

struct SerializedInodeMapEntry {
  1: i64 inodeNumber,
  2: i64 parentInode,
  3: string name,
  4: bool isUnlinked,
  5: i64 numFuseReferences,
  6: string hash,
  7: i32 mode,
}

struct SerializedInodeMap {
  2: list<SerializedInodeMapEntry> unloadedInodes,
}

struct SerializedFileHandleMap {}

struct SerializedMountInfo {
  1: string mountPath,
  2: string stateDirectory,
  // TODO: remove this field, it is no longer used
  3: list<string> bindMountPaths,
  // This binary blob is really a fuse_init_out instance.
  // We don't transcribe that into thrift because the layout
  // is system dependent and happens to be flat and thus is
  // suitable for reinterpret_cast to be used upon it to
  // access the struct once we've moved it across the process
  // boundary.  Note that takeover is always local to the same
  // machine and thus has the same endianness.
  4: binary connInfo, // fuse_init_out

  // Removed, do not use 5
  // 5: SerializedFileHandleMap fileHandleMap,

  6: SerializedInodeMap inodeMap,
}

union SerializedTakeoverData {
  1: list<SerializedMountInfo> mounts,
  2: string errorReason,
}
