#pragma once

#include <string_view>

namespace facebook::eden {

inline bool is_edenfs_fs_type(std::string_view type) {
  return type == "edenfs" || type == "edenfs_eden" ||
      type == "fuse.edenfs";
}

} // namespace facebook::eden
