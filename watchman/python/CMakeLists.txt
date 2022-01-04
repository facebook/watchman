# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(SETUP_PY "${CMAKE_CURRENT_SOURCE_DIR}/setup.py")
set(PYOUT "${CMAKE_CURRENT_BINARY_DIR}/build/pytimestamp")
set(DEPS
  "bin/watchman-make"
  "bin/watchman-replicate-subscription"
  "bin/watchman-wait"
  "pywatchman/__init__.py"
  "pywatchman/bser.c"
  "pywatchman/capabilities.py"
  "pywatchman/compat.py"
  "pywatchman/encoding.py"
  "pywatchman/load.py"
  "pywatchman/pybser.py"
  "pywatchman/windows.py"
)

add_custom_command(OUTPUT ${PYOUT}
  COMMAND ${CMAKE_COMMAND} -E env VSCMD_ARG_NO_EXT=1 ${Python3_EXECUTABLE} ${SETUP_PY} build
  COMMAND ${CMAKE_COMMAND} -E touch ${PYOUT}
  DEPENDS ${DEPS}
  COMMENT "Building pywatchman")

add_custom_target(pybuild ALL DEPENDS ${PYOUT})

install(CODE
  "execute_process(COMMAND ${Python3_EXECUTABLE} ${SETUP_PY} install
    --root $ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}
    RESULT_VARIABLE STATUS)
  if(NOT STATUS STREQUAL 0)
    message(FATAL_ERROR \"pywatchman install failed\")
  endif()")

add_subdirectory(pywatchman)

add_fb_python_executable(watchman-diag
  SOURCES
    bin/watchman-diag=__main__.py
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-diag)

add_fb_python_executable(watchman-make
  SOURCES
    bin/watchman-make=__main__.py
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-make)

add_fb_python_executable(watchman-replicate-subscription
  SOURCES
    bin/watchman-replicate-subscription=__main__.py
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-replicate-subscription)

add_fb_python_executable(watchman-wait
  SOURCES
    bin/watchman-wait=__main__.py
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-wait)
