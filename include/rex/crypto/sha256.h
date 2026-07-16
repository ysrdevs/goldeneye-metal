/**
 * SHA256 hashing utilities
 *
 * Provides SHA256 hashing for strings and files, used for cache invalidation.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace rex::crypto {

std::string sha256(std::string_view data);
std::string sha256_file(const std::filesystem::path& path);
std::string sha256_file(const std::filesystem::path& path, const std::function<bool()>& cancelled);

}  // namespace rex::crypto
