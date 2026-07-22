#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

class ImmediateCaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<std::string> CopyMessages() const {
    std::lock_guard lock(messages_mutex_);
    return messages_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    std::lock_guard lock(messages_mutex_);
    messages_.emplace_back(message.payload.begin(), message.payload.end());
  }

  void flush_() override {}

 private:
  mutable std::mutex messages_mutex_;
  std::vector<std::string> messages_;
};

class ScopedCategorySink {
 public:
  ScopedCategorySink(rex::LogCategoryId category, spdlog::sink_ptr sink)
      : category_(category), sink_(std::move(sink)) {
    rex::AddSink(category_, sink_);
  }

  ~ScopedCategorySink() { rex::RemoveSink(category_, sink_); }

 private:
  rex::LogCategoryId category_;
  spdlog::sink_ptr sink_;
};

class ScopedTempLog {
 public:
  ScopedTempLog()
      : path_(std::filesystem::temp_directory_path() /
              ("rex_sync_logging_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".log")) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ~ScopedTempLog() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("Synchronous fatal logging bypasses filters and persists before returning",
          "[core][logging]") {
  rex::InitLogging();
  const auto category = rex::log::krnl();
  auto* logger = rex::GetLoggerRaw(category);
  REQUIRE(logger != nullptr);

  auto capture = std::make_shared<ImmediateCaptureSink>();
  ScopedCategorySink registered_sink(category, capture);
  ScopedTempLog temp_log;
  auto file_capture = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      temp_log.path().string(), 1024 * 1024, 1, false);
  file_capture->set_level(spdlog::level::trace);
  file_capture->set_pattern("%v");
  ScopedCategorySink registered_file_sink(category, file_capture);
  const auto previous_level = logger->level();
  rex::SetCategoryLevel(category, spdlog::level::off);

  constexpr std::string_view sentinel =
      "[GUEST BUGCHECK] code=0xA1B2C3D4 params=(0x00000001, 0x00000002, 0x00000003, 0x00000004)";
  rex::LogSynchronously(category, spdlog::level::critical, sentinel);
  rex::SetCategoryLevel(category, previous_level);

  const auto messages = capture->CopyMessages();
  REQUIRE(std::count(messages.begin(), messages.end(), sentinel) == 1);

  std::ifstream persisted_log(temp_log.path(), std::ios::binary);
  REQUIRE(persisted_log.is_open());
  const std::string persisted((std::istreambuf_iterator<char>(persisted_log)),
                              std::istreambuf_iterator<char>());
  REQUIRE(persisted.find(sentinel) != std::string::npos);
}
