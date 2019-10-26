add_fb_python_unittest(bser_js
  SOURCES
    test_bser.py

  WORKING_DIRECTORY
    ${CMAKE_BINARY_DIR}

  ENV
    "YARN_PATH=${YARN}"
    "NODE_BIN=${NODE}"
    "WATCHMAN_SRC_DIR=${CMAKE_SOURCE_DIR}"
)
