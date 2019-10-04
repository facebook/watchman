add_subdirectory(pywatchman)

add_fb_python_executable(watchman-wait
  SOURCES
    bin/watchman-wait=__main__.py
  DEPENDS
    pywatchman
)
