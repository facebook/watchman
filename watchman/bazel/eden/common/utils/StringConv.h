#pragma once

#include <string>
#include <string_view>

namespace facebook::eden {

template <typename String>
String wideToMultibyteString(std::wstring_view value) {
  String result;
  result.reserve(value.size());
  for (wchar_t ch : value) {
    result.push_back(static_cast<typename String::value_type>(ch));
  }
  return result;
}

} // namespace facebook::eden
