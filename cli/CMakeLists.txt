# The watchman_client build was broken by a Tokio upgrade:
# See https://github.com/facebook/watchman/issues/898
if(NOT WIN32)
  rust_executable(watchmanctl)
  install_rust_executable(watchmanctl)
endif()
