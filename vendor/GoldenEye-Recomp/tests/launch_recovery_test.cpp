#include "ge_launch_recovery.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool Write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return output.good();
}

}  // namespace

int main() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() /
                    ("goldeneye-launch-recovery-test-" + unique);
  const auto config = root / "GoldenEyeMetal.toml";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return 1;
  }
  auto cleanup = [&] { std::filesystem::remove_all(root, ec); };

  auto startup = ge::launch_recovery::PrepareStartup(root, config);
  if (startup.interrupted_run || startup.restored_safe_mode_config || !startup.warning.empty()) {
    cleanup();
    return 2;
  }

  std::string error;
  if (!ge::launch_recovery::BeginRun(root, &error)) {
    cleanup();
    return 3;
  }
#if !defined(_WIN32)
  struct stat run_marker_status {};
  if (::stat((root / ".goldeneye-metal-run-active").c_str(), &run_marker_status) != 0 ||
      (run_marker_status.st_mode & 077u) != 0) {
    cleanup();
    return 4;
  }
#endif
  startup = ge::launch_recovery::PrepareStartup(root, config);
  if (!startup.interrupted_run) {
    cleanup();
    return 5;
  }
  if (!ge::launch_recovery::EndRun(root)) {
    cleanup();
    return 6;
  }
  startup = ge::launch_recovery::PrepareStartup(root, config);
  if (startup.interrupted_run) {
    cleanup();
    return 7;
  }

  constexpr std::string_view kOriginalConfig =
      "fullscreen = true\nmetal_output_scaler = \"metalfx\"\n";
  constexpr std::string_view kTemporarySafeConfig =
      "fullscreen = false\nmetal_output_scaler = \"bilinear\"\n";
  if (!Write(config, kOriginalConfig) ||
      !ge::launch_recovery::BeginSafeMode(root, config, &error) ||
      !Write(config, kTemporarySafeConfig)) {
    cleanup();
    return 8;
  }
#if !defined(_WIN32)
  struct stat safe_backup_status {};
  if (::stat((root / ".goldeneye-metal-safe-mode-config-backup").c_str(),
             &safe_backup_status) != 0 ||
      (safe_backup_status.st_mode & 077u) != 0) {
    cleanup();
    return 9;
  }
#endif
  startup = ge::launch_recovery::PrepareStartup(root, config);
  if (!startup.restored_safe_mode_config || !startup.warning.empty() ||
      Read(config) != kOriginalConfig) {
    cleanup();
    return 10;
  }

  std::filesystem::remove(config, ec);
  if (!ge::launch_recovery::BeginSafeMode(root, config, &error) ||
      !Write(config, kTemporarySafeConfig)) {
    cleanup();
    return 11;
  }
  startup = ge::launch_recovery::PrepareStartup(root, config);
  if (!startup.restored_safe_mode_config || std::filesystem::exists(config)) {
    cleanup();
    return 12;
  }

  // A Safe Mode crash leaves both markers. The next startup must restore the
  // exact config before reporting the interrupted run.
  if (!Write(config, kOriginalConfig) ||
      !ge::launch_recovery::BeginSafeMode(root, config, &error) ||
      !ge::launch_recovery::BeginRun(root, &error) || !Write(config, kTemporarySafeConfig)) {
    cleanup();
    return 13;
  }
  startup = ge::launch_recovery::PrepareStartup(root, config);
  if (!startup.interrupted_run || !startup.restored_safe_mode_config ||
      Read(config) != kOriginalConfig) {
    cleanup();
    return 14;
  }
  if (!ge::launch_recovery::EndRun(root)) {
    cleanup();
    return 15;
  }

#if !defined(_WIN32)
  const auto external = root / "external";
  std::filesystem::remove(config, ec);
  if (!Write(external, "do-not-touch") ||
      ::symlink(external.c_str(), config.c_str()) != 0 ||
      ge::launch_recovery::BeginSafeMode(root, config, &error) || Read(external) != "do-not-touch") {
    cleanup();
    return 16;
  }
#endif

  cleanup();
  return 0;
}
