/**
 * SHA256 hashing utilities implementation
 *
 * Uses picosha2 header-only library for SHA256 computation.
 */

#include "thirdparty/picosha2/picosha2.h"

#include <fstream>
#include <vector>

#include <rex/crypto/sha256.h>

namespace rex::crypto {

std::string sha256(std::string_view data) {
  std::string hash_hex;
  picosha2::hash256_hex_string(data.begin(), data.end(), hash_hex);
  return hash_hex;
}

std::string sha256_file(const std::filesystem::path& path) {
  return sha256_file(path, {});
}

std::string sha256_file(const std::filesystem::path& path, const std::function<bool()>& cancelled) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }

  // Hash through the stream instead of materializing the entire file. Game
  // backups can be hundreds of megabytes, and validation must have a small,
  // predictable memory footprint.
  std::vector<picosha2::byte_t> buffer(1024 * 1024);
  picosha2::hash256_one_by_one hasher;
  while (file) {
    if (cancelled && cancelled()) {
      return "";
    }
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    size_t size = static_cast<size_t>(file.gcount());
    hasher.process(buffer.begin(), buffer.begin() + size);
  }
  if (file.bad() || (file.fail() && !file.eof()) || (cancelled && cancelled())) {
    return "";
  }
  hasher.finish();
  return picosha2::get_hash_hex_string(hasher);
}

}  // namespace rex::crypto
