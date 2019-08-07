add_thrift_cpp2_library(
  fb303_thrift_cpp
  fb303/if/fb303.thrift
  SERVICES
    FacebookService
)

add_library(
  common_stats STATIC
    stats/ServiceData.cpp
)
set_property(
  TARGET common_stats
  PROPERTY PUBLIC_HEADER
    stats/ExportedHistogramMap.h
    stats/ExportedHistogramMapImpl.h
    stats/ExportedStatMap.h
    stats/ExportedStatMapImpl.h
    stats/ExportType.h
    stats/DynamicCounters.h
    stats/MonotonicCounter.h
    stats/ServiceData.h
    stats/ThreadCachedServiceData.h
    stats/ThreadLocalStats.h
)
target_include_directories(
  common_stats
  PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(
  common_stats
  PUBLIC
    Folly::folly
)
