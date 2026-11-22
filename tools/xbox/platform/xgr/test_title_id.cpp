#include <cstdio>
#include <optional>

#include "runtime/title_id.h"

int main() {
  struct TestCase {
    const char* name;
    const char* config;
    std::optional<uint32_t> expected;
  };
  const TestCase cases[] = {
      {"plain hexadecimal", "<game><titleid>12AB</titleid></game>", 0x12ABu},
      {"leading whitespace and prefix", "<titleid> \t+0x1a</titleid>", 0x1Au},
      {"zero", "<titleid>0</titleid>", 0u},
      {"maximum title ID", "<titleid>ffffffff</titleid>", UINT32_MAX},
      {"one above the maximum", "<titleid>100000000</titleid>", std::nullopt},
      {"larger than 64 bits", "<titleid>10000000000000000</titleid>", std::nullopt},
      {"negative ID", "<titleid>-1</titleid>", std::nullopt},
      {"junk after hex digits", "<titleid>12g</titleid>", std::nullopt},
      {"prefix without digits", "<titleid>0x</titleid>", std::nullopt},
      {"empty ID", "<titleid></titleid>", std::nullopt},
      {"missing title ID", "<game></game>", std::nullopt},
  };

  int failures = 0;
  for (const auto& test : cases) {
    constexpr uint32_t untouched = 0x12345678;
    uint32_t title = untouched;
    const bool parsed = gtx::xgr::parse_title_id(test.config, &title);
    const bool passed = parsed == test.expected.has_value() && title == test.expected.value_or(untouched);
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
    failures += !passed;
  }
  return failures ? 1 : 0;
}
