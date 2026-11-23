#include "core/GraphConfig.hpp"
#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
class SeedFixture {
public:
  SeedFixture() {
    auto pattern = (std::filesystem::temp_directory_path() / "mam-seed-config-XXXXXX").string();
    const char* created = mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("Cannot create seed test directory");
    directory_ = created;
  }
  ~SeedFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
  SeedFixture(const SeedFixture&) = delete;
  SeedFixture& operator=(const SeedFixture&) = delete;

  GraphSpec load(const std::string& token) const {
    return loadDocument("{\"kind\":\"rack\",\"nodes\":[],\"randomSeed\":" + token + "}");
  }
  GraphSpec loadDocument(const std::string& document) const {
    const auto file = directory_ / "rack.json";
    {
      std::ofstream output(file);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << document;
    }
    return loadGraphSpecFromJsonFile(file.string());
  }
private:
  std::filesystem::path directory_;
};
}

TEST_CASE("graph seed accepts integer boundaries and preserves unspecified zero") {
  SeedFixture fixture;
  CHECK(fixture.loadDocument("{\"kind\":\"rack\",\"nodes\":[]}").randomSeed == 0);
  for (const auto& [token, expected] : std::vector<std::pair<const char*, uint32_t>>{
      {"0", 0u}, {"1", 1u}, {"2147483648", 2147483648u}, {"4294967295", 4294967295u}}) {
    INFO(token);
    CHECK(fixture.load(token).randomSeed == expected);
  }
}

TEST_CASE("graph seed rejects numeric coercion and values outside uint32") {
  SeedFixture fixture;
  for (const auto* token : {"true", "false", "null", "\"12\"", "\"\"", "[]", "{}",
      "-1", "-4294967295", "-9223372036854775808", "1.0", "0.0", "1.25", "1e2",
      "4294967296", "9223372036854775807", "18446744073709551615", "18446744073709551616"}) {
    INFO(token);
    CHECK_THROWS_WITH_AS(fixture.load(token), "randomSeed must be an integer in 0..4294967295", std::runtime_error);
  }
  // JSON syntax/number-overflow failures are also rejected by the parser.
  for (const auto* token : {"+1", "01", "1e400"}) {
    INFO(token);
    CHECK_THROWS(fixture.load(token));
  }
}
