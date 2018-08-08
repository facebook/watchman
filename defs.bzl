COMPILER_FLAGS = [
    "-Wunused-variable",
    "-DWATCHMAN_FACEBOOK_INTERNAL",
]

def sysdep_watcher():
    if native.host_info().os.is_linux:
        cpp_library(
            name = "eden_watcher",
            srcs = [
                "cmds/heapprof.cpp",
                "watcher/eden.cpp",
            ],
            compiler_flags = COMPILER_FLAGS + ["-DUSE_JEMALLOC"],
            # We use constructors to declare commands rather than maintaining
            # static tables of things.  Ensure that they don't get stripped
            # out of the final binary!
            link_whole = True,
            undefined_symbols = True,  # TODO(T23121628): fix deps and remove
            deps = [
                ":err",
                ":headers",
                "//eden/fs/service:thrift-streaming-cpp2",
                "//folly:string",
                "//watchman/thirdparty/wildmatch:wildmatch",
            ],
        )

        # Linux specific watcher module
        cpp_library(
            name = "sysdep_watcher",
            srcs = ["watcher/inotify.cpp"],
            compiler_flags = COMPILER_FLAGS,
            # We use constructors to declare commands rather than maintaining
            # static tables of things.  Ensure that they don't get stripped
            # out of the final binary!
            link_whole = True,
            undefined_symbols = True,  # TODO(T23121628): fix deps and remove
            deps = [
                ":eden_watcher",
                ":err",
                ":headers",
            ],
        )
    elif native.host_info().os.is_mac:
        # mac specific watcher module
        cpp_library(
            name = "sysdep_watcher",
            srcs = [
                "watcher/fsevents.cpp",
                "watcher/kqueue.cpp",
            ],
            compiler_flags = COMPILER_FLAGS,
            # We use constructors to declare commands rather than maintaining
            # static tables of things.  Ensure that they don't get stripped
            # out of the final binary!
            link_whole = True,
            deps = [":headers"],
        )
    elif native.host_info().os.is_windows:
        # windows specific watcher module
        cpp_library(
            name = "sysdep_watcher",
            srcs = ["watcher/win32.cpp"],
            compiler_flags = COMPILER_FLAGS,
            # We use constructors to declare commands rather than maintaining
            # static tables of things.  Ensure that they don't get stripped
            # out of the final binary!
            link_whole = True,
            deps = [":headers"],
        )
