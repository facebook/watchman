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
