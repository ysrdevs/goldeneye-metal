/**
 * Unit tests for SHA256 hashing utilities
 *
 * Tests SHA256 computation for strings and files.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/crypto/sha256.h>

#include <chrono>
#include <filesystem>
#include <fstream>

TEST_CASE("sha256 empty string", "[sha256]") {
  auto hash = rex::crypto::sha256("");
  CHECK(hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 hello world", "[sha256]") {
  auto hash = rex::crypto::sha256("hello world");
  CHECK(hash == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_CASE("sha256 file streams content and supports cancellation", "[sha256]") {
  auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path =
      std::filesystem::temp_directory_path() / ("rex-sha256-test-" + std::to_string(unique));
  {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    output << "hello world";
  }

  CHECK(rex::crypto::sha256_file(path) ==
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
  CHECK(rex::crypto::sha256_file(path, [] { return true; }).empty());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}
