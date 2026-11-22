#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace gtx::xgr {

inline bool parse_title_id(std::string_view config, uint32_t* out) {
  constexpr std::string_view tag = "<titleid>";
  const auto start = config.find(tag);
  if (start == std::string_view::npos) return false;
  config.remove_prefix(start + tag.size());

  const auto endTag = config.find('<');
  if (endTag == std::string_view::npos) return false;
  auto value = config.substr(0, endTag);
  const auto first = value.find_first_not_of(" \t\n\r\f\v");
  if (first == std::string_view::npos) return false;
  value.remove_prefix(first);
  if (value.starts_with('+')) value.remove_prefix(1);
  if (value.starts_with("0x")) value.remove_prefix(2);

  uint32_t title;
  const auto end = value.data() + value.size();
  const auto result = std::from_chars(value.data(), end, title, 16);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  *out = title;
  return true;
}

}
