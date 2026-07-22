#include "ge_save_manager.h"

#include <rex/system/xam/content_manager.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using rex::system::XContentType;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

const std::filesystem::path kSavePackage = "B13EBABEBABEBABE/584108A9/00000001/beansave.dat";
const std::filesystem::path kSavePayload = kSavePackage / "beansave.dat";
const std::filesystem::path kSaveThumbnail = kSavePackage / "__thumbnail.png";
const std::filesystem::path kSaveHeader =
    "B13EBABEBABEBABE/584108A9/Headers/00000001/beansave.dat.header";
const std::filesystem::path kProfileSetting = "584108A9/profile/User/63E83FFF";

class TemporaryTree {
 public:
  TemporaryTree() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    path = std::filesystem::temp_directory_path() / ("goldeneye-save-manager-test-" + unique);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }

  ~TemporaryTree() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

bool Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return output.good();
}

bool WriteText(const std::filesystem::path& path, std::string_view text) {
  return Write(path, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

std::vector<uint8_t> MakeHeader() {
  XCONTENT_AGGREGATE_DATA header{};
  header.device_id = 1;
  header.content_type = XContentType::kSavedGame;
  header.set_display_name(u"GoldenEye Save");
  header.set_file_name("beansave.dat");
  header.xuid = 0xB13EBABEBABEBABEull;
  header.title_id = 0x584108A9u;

  std::vector<uint8_t> bytes(sizeof(header));
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

bool CreateFixture(const std::filesystem::path& root, uint8_t payload_byte) {
  static const std::vector<uint8_t> kPngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  return Write(root / kSavePayload, std::vector<uint8_t>(512, payload_byte)) &&
         Write(root / kSaveThumbnail, kPngSignature) && Write(root / kSaveHeader, MakeHeader()) &&
         Write(root / kProfileSetting, {1, 3, 3, 7});
}

int Fail(int code, std::string_view message) {
  std::cerr << "save_manager_test: " << message << '\n';
  return code;
}

const std::array<std::pair<std::filesystem::path, std::string_view>, 5> kUnrelatedSentinels = {{
    {"Game Data/keep-me.txt", "game-data-sentinel"},
    {"Cache/keep-me.bin", "cache-sentinel"},
    {"Logs/keep-me.log", "logs-sentinel"},
    {"GoldenEyeMetal.toml", "config-sentinel"},
    {"DEADBEEF/profile/Other/63E83FFF", "foreign-title-sentinel"},
}};

bool WriteSentinels(const std::filesystem::path& root) {
  for (const auto& [relative, contents] : kUnrelatedSentinels) {
    if (!WriteText(root / relative, contents)) {
      return false;
    }
  }
  return true;
}

bool SentinelsMatch(const std::filesystem::path& root) {
  for (const auto& [relative, contents] : kUnrelatedSentinels) {
    if (Read(root / relative) != std::vector<uint8_t>(contents.begin(), contents.end())) {
      return false;
    }
  }
  return true;
}

bool FullStateMatches(const std::filesystem::path& root, uint8_t payload,
                      const std::vector<uint8_t>& profile) {
  ge::save::Snapshot snapshot;
  auto status = ge::save::Discover(root, &snapshot);
  return status && snapshot.has_save_game && snapshot.has_profile_settings &&
         Read(root / kSavePayload) == std::vector<uint8_t>(512, payload) &&
         Read(root / kProfileSetting) == profile;
}

bool EmptyStateMatches(const std::filesystem::path& root) {
  ge::save::Snapshot snapshot;
  auto status = ge::save::Discover(root, &snapshot);
  return status && !snapshot;
}

#if !defined(_WIN32)
std::optional<bool> JournalCommitted(const std::filesystem::path& root) {
  const auto journal = root / ".goldeneye-save-transaction";
  if (!std::filesystem::exists(journal)) {
    return std::nullopt;
  }
  const auto bytes = Read(journal);
  if (bytes.size() < 28) {
    return false;
  }
  const uint32_t state =
      static_cast<uint32_t>(bytes[24]) | (static_cast<uint32_t>(bytes[25]) << 8) |
      (static_cast<uint32_t>(bytes[26]) << 16) | (static_cast<uint32_t>(bytes[27]) << 24);
  return state == 2;
}

using SetupCrashCase = std::function<bool(const std::filesystem::path&, std::filesystem::path*,
                                          std::filesystem::path*)>;
using RunCrashOperation =
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&)>;
using VerifyCrashState = std::function<bool(const std::filesystem::path&)>;

bool RunCrashMatrix(const SetupCrashCase& setup, const RunCrashOperation& operation,
                    const VerifyCrashState& verify_precommit,
                    const VerifyCrashState& verify_committed) {
  bool saw_precommit = false;
  bool saw_committed = false;
  bool saw_completion = false;
  for (int checkpoint = 0; checkpoint < 20; ++checkpoint) {
    TemporaryTree temporary;
    std::filesystem::path root;
    std::filesystem::path quarantine;
    if (!setup(temporary.path, &root, &quarantine)) {
      return false;
    }

    const pid_t child = fork();
    if (child < 0) {
      return false;
    }
    if (child == 0) {
      ge::save::testing::SetCrashAfterCheckpoint(checkpoint);
      _exit(operation(root, quarantine) ? 0 : 85);
    }
    int wait_status = 0;
    if (waitpid(child, &wait_status, 0) != child || !WIFEXITED(wait_status)) {
      return false;
    }
    const int exit_code = WEXITSTATUS(wait_status);
    if (exit_code != 0 && exit_code != 86) {
      return false;
    }

    const auto committed = JournalCommitted(root);
    const bool expected_committed = !committed.has_value() || *committed;
    auto recovery = ge::save::RecoverInterruptedTransaction(root);
    if (!recovery || recovery.recovered != committed.has_value()) {
      return false;
    }
    auto second_recovery = ge::save::RecoverInterruptedTransaction(root);
    if (!second_recovery || second_recovery.recovered) {
      return false;
    }
    if (!(expected_committed ? verify_committed(root) : verify_precommit(root)) ||
        !SentinelsMatch(root)) {
      return false;
    }
    saw_precommit |= !expected_committed;
    saw_committed |= expected_committed;
    if (exit_code == 0) {
      saw_completion = true;
      break;
    }
  }
  return saw_precommit && saw_committed && saw_completion;
}
#endif

}  // namespace

int main() {
  TemporaryTree temporary;
  const auto root = temporary.path / "GoldenEye Metal";
  const auto archive = temporary.path / "GoldenEye-test.gesave";
  const auto tampered_archive = temporary.path / "GoldenEye-tampered.gesave";
  const auto foreign_archive = temporary.path / "GoldenEye-foreign.gesave";
  if (!CreateFixture(root, 0x31) || !WriteSentinels(root)) {
    return Fail(1, "could not create the isolated fixture");
  }

  ge::save::Snapshot snapshot;
  auto status = ge::save::Discover(root, &snapshot);
  if (!status || !snapshot.has_save_game || !snapshot.has_profile_settings ||
      snapshot.file_count != 4) {
    return Fail(2, "valid save/profile data was not discovered");
  }

  ge::save::BackupInfo backup_info;
  status = ge::save::CreateBackup(root, archive, &backup_info);
  if (!status || backup_info.format_version != 1 || backup_info.file_count != snapshot.file_count ||
      backup_info.byte_count != snapshot.byte_count) {
    return Fail(3, "backup creation or read-back verification failed");
  }
  ge::save::BackupInfo inspected_info;
  status = ge::save::InspectBackup(archive, &inspected_info);
  if (!status || inspected_info.file_count != backup_info.file_count ||
      inspected_info.byte_count != backup_info.byte_count) {
    return Fail(4, "the generated backup could not be inspected");
  }
  status = ge::save::CreateBackup(root, archive);
  if (status || status.error != ge::save::Error::kConflict) {
    return Fail(5, "backup creation overwrote an existing destination");
  }
  status = ge::save::CreateBackup(root, root / "unsafe.gesave");
  if (status || status.error != ge::save::Error::kInvalidArgument) {
    return Fail(6, "backup creation accepted a destination inside app data");
  }
  std::error_code ec;
#if !defined(_WIN32)
  const auto alias_parent = root / "Backup Alias Target";
  const auto root_alias = temporary.path / "root-alias";
  std::filesystem::create_directories(alias_parent, ec);
  std::filesystem::create_directory_symlink(root, root_alias, ec);
  if (ec) {
    return Fail(30, "could not create the ancestor-symlink backup fixture");
  }
  status = ge::save::CreateBackup(root, root_alias / "Backup Alias Target/unsafe.gesave");
  if (status || status.error != ge::save::Error::kInvalidArgument ||
      std::filesystem::exists(alias_parent / "unsafe.gesave")) {
    return Fail(31, "backup destination escaped the outside-root check through a symlink");
  }
#endif

  const auto original_payload = Read(root / kSavePayload);
  const std::vector<uint8_t> modified_payload(512, 0x52);
  if (!Write(root / kSavePayload, modified_payload)) {
    return Fail(7, "could not modify the fixture before restore");
  }
  auto mutation = ge::save::RestoreBackup(root, archive);
  if (!mutation || mutation.quarantine_path.empty() ||
      Read(root / kSavePayload) != original_payload) {
    return Fail(8, "restore did not install the backup and quarantine old data");
  }
  const auto replaced_quarantine = mutation.quarantine_path;
  mutation = ge::save::UndoQuarantine(root, replaced_quarantine);
  if (!mutation || mutation.quarantine_path.empty() ||
      Read(root / kSavePayload) != modified_payload ||
      std::filesystem::exists(replaced_quarantine)) {
    return Fail(9, "restore undo did not transactionally recover replaced data");
  }
  const auto redo_quarantine = mutation.quarantine_path;
  mutation = ge::save::UndoQuarantine(root, redo_quarantine);
  if (!mutation || mutation.quarantine_path.empty() ||
      Read(root / kSavePayload) != original_payload || !SentinelsMatch(root)) {
    return Fail(32, "restore undo quarantine could not transactionally redo the restore");
  }

  mutation = ge::save::ResetToFresh(root);
  if (!mutation || mutation.quarantine_path.empty()) {
    return Fail(10, "reset did not quarantine the managed save data");
  }
  const auto reset_quarantine = mutation.quarantine_path;
  status = ge::save::Discover(root, &snapshot);
  if (!status || snapshot || !SentinelsMatch(root)) {
    return Fail(11, "reset touched unrelated app data or left managed data live");
  }
  std::filesystem::create_directories(root / kSavePackage, ec);
  mutation = ge::save::UndoQuarantine(root, reset_quarantine);
  if (mutation || mutation.status.error != ge::save::Error::kConflict) {
    return Fail(33, "reset undo overwrote a current managed unit");
  }
  std::filesystem::remove(root / kSavePackage, ec);
  mutation = ge::save::UndoQuarantine(root, reset_quarantine);
  if (!mutation || Read(root / kSavePayload) != original_payload || !SentinelsMatch(root)) {
    return Fail(12, "reset undo did not restore the exact save data");
  }

  auto archive_bytes = Read(archive);
  if (archive_bytes.empty()) {
    return Fail(13, "could not read the backup for tamper tests");
  }
  archive_bytes.back() ^= 0x80;
  if (!Write(tampered_archive, archive_bytes)) {
    return Fail(14, "could not create the tampered backup fixture");
  }
  status = ge::save::InspectBackup(tampered_archive);
  if (status || status.error != ge::save::Error::kInvalidArchive) {
    return Fail(15, "tampered backup passed integrity validation");
  }
  const auto before_bad_restore = Read(root / kSavePayload);
  mutation = ge::save::RestoreBackup(root, tampered_archive);
  if (mutation || Read(root / kSavePayload) != before_bad_restore) {
    return Fail(16, "invalid backup changed live save data");
  }

  archive_bytes = Read(archive);
  const std::string title_component = "584108A9";
  auto title = std::search(archive_bytes.begin(), archive_bytes.end(), title_component.begin(),
                           title_component.end());
  if (title == archive_bytes.end()) {
    return Fail(17, "could not locate archive path for foreign-title test");
  }
  *(title + 7) = 'B';
  if (!Write(foreign_archive, archive_bytes)) {
    return Fail(18, "could not create the foreign-title archive fixture");
  }
  status = ge::save::InspectBackup(foreign_archive);
  if (status || status.error != ge::save::Error::kInvalidArchive) {
    return Fail(19, "backup accepted a foreign title path");
  }

  const auto unknown_setting = root / "584108A9/profile/User/DEADBEEF";
  if (!Write(unknown_setting, {9})) {
    return Fail(20, "could not create unknown profile setting fixture");
  }
  status = ge::save::Discover(root, &snapshot);
  if (status || status.error != ge::save::Error::kUnsafeLayout) {
    return Fail(21, "unknown profile setting was accepted");
  }
  mutation = ge::save::ResetToFresh(root);
  if (mutation || mutation.status.error != ge::save::Error::kUnsafeLayout ||
      Read(root / kSavePayload) != original_payload) {
    return Fail(22, "reset did not refuse an unsafe managed layout");
  }
  std::filesystem::remove(unknown_setting, ec);

  std::filesystem::remove(root / kSaveHeader, ec);
  status = ge::save::Discover(root, &snapshot);
  if (status || status.error != ge::save::Error::kUnsafeLayout) {
    return Fail(23, "an incomplete save package was accepted");
  }
  if (!Write(root / kSaveHeader, MakeHeader())) {
    return Fail(24, "could not repair the header fixture");
  }

#if !defined(_WIN32)
  const auto external = temporary.path / "external-setting";
  if (!Write(external, {4, 2}) || !std::filesystem::remove(root / kProfileSetting, ec)) {
    return Fail(25, "could not prepare the symlink fixture");
  }
  std::filesystem::create_symlink(external, root / kProfileSetting, ec);
  if (ec) {
    return Fail(26, "could not create the symlink fixture");
  }
  status = ge::save::Discover(root, &snapshot);
  if (status || status.error != ge::save::Error::kUnsafeLayout ||
      Read(external) != std::vector<uint8_t>({4, 2})) {
    return Fail(27, "managed symlink was accepted or its target was changed");
  }
  std::filesystem::remove(root / kProfileSetting, ec);
  if (!Write(root / kProfileSetting, {1, 3, 3, 7})) {
    return Fail(28, "could not repair the symlink fixture");
  }
#endif

  status = ge::save::Discover(root, &snapshot);
  if (!status || !snapshot.has_save_game || !snapshot.has_profile_settings) {
    return Fail(29, "the repaired fixture did not validate");
  }

  TemporaryTree empty_setting_temporary;
  const auto empty_setting_root = empty_setting_temporary.path / "GoldenEye Metal";
  const auto empty_setting_archive = empty_setting_temporary.path / "empty-profile-setting.gesave";
  if (!Write(empty_setting_root / "584108A9/profile/User/63E83FFE", {}) ||
      !ge::save::CreateBackup(empty_setting_root, empty_setting_archive, &backup_info) ||
      backup_info.file_count != 1 || backup_info.byte_count != 0 ||
      !ge::save::InspectBackup(empty_setting_archive)) {
    return Fail(39, "an empty title-profile setting did not round-trip safely");
  }

#if !defined(_WIN32)
  TemporaryTree rollback_temporary;
  const auto rollback_root = rollback_temporary.path / "GoldenEye Metal";
  const auto profile_only_root = rollback_temporary.path / "profile-only";
  const auto profile_only_archive = rollback_temporary.path / "profile-only.gesave";
  const std::vector<uint8_t> post_restore_profile = {8, 6, 7, 5, 3, 0, 9};
  if (!CreateFixture(rollback_root, 0x19) ||
      !Write(profile_only_root / kProfileSetting, post_restore_profile) ||
      !ge::save::CreateBackup(profile_only_root, profile_only_archive)) {
    return Fail(34, "could not create the restore-undo rollback fixture");
  }
  mutation = ge::save::RestoreBackup(rollback_root, profile_only_archive);
  if (!mutation || mutation.quarantine_path.empty() ||
      std::filesystem::exists(rollback_root / kSavePayload) ||
      Read(rollback_root / kProfileSetting) != post_restore_profile) {
    return Fail(35, "could not establish a profile-only restored snapshot");
  }
  const auto rollback_quarantine = mutation.quarantine_path;
  const auto blocked_save_parent = rollback_root / "B13EBABEBABEBABE/584108A9/00000001";
  if (::chmod(blocked_save_parent.c_str(), 0500) != 0) {
    return Fail(36, "could not make the old-save restore fail deterministically");
  }
  mutation = ge::save::UndoQuarantine(rollback_root, rollback_quarantine);
  const bool undo_failed = !mutation;
  const bool current_rolled_back = !std::filesystem::exists(rollback_root / kSavePayload) &&
                                   Read(rollback_root / kProfileSetting) == post_restore_profile;
  ::chmod(blocked_save_parent.c_str(), 0700);
  if (!undo_failed || !current_rolled_back || !std::filesystem::exists(rollback_quarantine)) {
    return Fail(37, "failed restore undo did not roll the current snapshot back intact");
  }
  mutation = ge::save::UndoQuarantine(rollback_root, rollback_quarantine);
  if (!mutation || Read(rollback_root / kSavePayload) != std::vector<uint8_t>(512, 0x19) ||
      Read(rollback_root / kProfileSetting) != std::vector<uint8_t>({1, 3, 3, 7})) {
    return Fail(38, "restore undo could not be retried after rollback");
  }

  TemporaryTree crash_archive_temporary;
  const auto crash_archive_source = crash_archive_temporary.path / "source";
  const auto crash_archive = crash_archive_temporary.path / "new-save.gesave";
  const std::vector<uint8_t> old_profile = {1, 2, 3, 4};
  const std::vector<uint8_t> new_profile = {9, 8, 7, 6};
  if (!CreateFixture(crash_archive_source, 0x72) ||
      !Write(crash_archive_source / kProfileSetting, new_profile) ||
      !ge::save::CreateBackup(crash_archive_source, crash_archive)) {
    return Fail(40, "could not create the crash-matrix backup");
  }

  const auto setup_old = [&](const std::filesystem::path& base, std::filesystem::path* case_root,
                             std::filesystem::path* quarantine) {
    *case_root = base / "GoldenEye Metal";
    quarantine->clear();
    return CreateFixture(*case_root, 0x61) && Write(*case_root / kProfileSetting, old_profile) &&
           WriteSentinels(*case_root);
  };
  const auto old_state = [&](const std::filesystem::path& case_root) {
    return FullStateMatches(case_root, 0x61, old_profile);
  };
  const auto new_state = [&](const std::filesystem::path& case_root) {
    return FullStateMatches(case_root, 0x72, new_profile);
  };

  if (!RunCrashMatrix(
          setup_old,
          [](const auto& case_root, const auto&) {
            return static_cast<bool>(ge::save::ResetToFresh(case_root));
          },
          old_state, [](const auto& case_root) { return EmptyStateMatches(case_root); })) {
    return Fail(41, "reset crash checkpoints did not reconcile atomically");
  }

  if (!RunCrashMatrix(
          setup_old,
          [&](const auto& case_root, const auto&) {
            return static_cast<bool>(ge::save::RestoreBackup(case_root, crash_archive));
          },
          old_state, new_state)) {
    return Fail(42, "restore crash checkpoints did not reconcile atomically");
  }

  const auto setup_reset_undo = [&](const std::filesystem::path& base,
                                    std::filesystem::path* case_root,
                                    std::filesystem::path* quarantine) {
    if (!setup_old(base, case_root, quarantine)) {
      return false;
    }
    auto reset = ge::save::ResetToFresh(*case_root);
    if (!reset) {
      return false;
    }
    *quarantine = reset.quarantine_path;
    return true;
  };
  if (!RunCrashMatrix(
          setup_reset_undo,
          [](const auto& case_root, const auto& quarantine) {
            return static_cast<bool>(ge::save::UndoQuarantine(case_root, quarantine));
          },
          [](const auto& case_root) { return EmptyStateMatches(case_root); }, old_state)) {
    return Fail(43, "reset-undo crash checkpoints did not reconcile atomically");
  }

  const auto setup_restore_undo = [&](const std::filesystem::path& base,
                                      std::filesystem::path* case_root,
                                      std::filesystem::path* quarantine) {
    if (!setup_old(base, case_root, quarantine)) {
      return false;
    }
    auto restore = ge::save::RestoreBackup(*case_root, crash_archive);
    if (!restore || restore.quarantine_path.empty()) {
      return false;
    }
    *quarantine = restore.quarantine_path;
    return true;
  };
  if (!RunCrashMatrix(
          setup_restore_undo,
          [](const auto& case_root, const auto& quarantine) {
            return static_cast<bool>(ge::save::UndoQuarantine(case_root, quarantine));
          },
          new_state, old_state)) {
    return Fail(44, "restore-undo crash checkpoints did not reconcile atomically");
  }

  TemporaryTree lock_temporary;
  const auto lock_root = lock_temporary.path / "GoldenEye Metal";
  if (!CreateFixture(lock_root, 0x44)) {
    return Fail(45, "could not create the cross-process lock fixture");
  }
  ge::save::Snapshot lock_snapshot;
  if (!ge::save::Discover(lock_root, &lock_snapshot)) {
    return Fail(46, "could not initialize the save-operation lock");
  }
  int ready_pipe[2] = {-1, -1};
  int release_pipe[2] = {-1, -1};
  if (pipe(ready_pipe) != 0 || pipe(release_pipe) != 0) {
    return Fail(47, "could not create lock-test pipes");
  }
  const pid_t lock_child = fork();
  if (lock_child == 0) {
    close(ready_pipe[0]);
    close(release_pipe[1]);
    int descriptor =
        open((lock_root / ".goldeneye-save-manager.lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    const bool locked = descriptor >= 0 && flock(descriptor, LOCK_EX) == 0;
    const char signal = locked ? '1' : '0';
    write(ready_pipe[1], &signal, 1);
    char release = 0;
    read(release_pipe[0], &release, 1);
    _exit(locked ? 0 : 85);
  }
  close(ready_pipe[1]);
  close(release_pipe[0]);
  char ready = 0;
  if (lock_child < 0 || read(ready_pipe[0], &ready, 1) != 1 || ready != '1') {
    return Fail(48, "the second process could not hold the save lock");
  }
  status = ge::save::Discover(lock_root, &lock_snapshot);
  const char release = '1';
  write(release_pipe[1], &release, 1);
  int lock_wait_status = 0;
  waitpid(lock_child, &lock_wait_status, 0);
  close(ready_pipe[0]);
  close(release_pipe[1]);
  if (status || status.error != ge::save::Error::kConflict || !WIFEXITED(lock_wait_status) ||
      WEXITSTATUS(lock_wait_status) != 0) {
    return Fail(49, "save operations were not serialized across processes");
  }

  TemporaryTree journal_symlink_temporary;
  const auto journal_symlink_root = journal_symlink_temporary.path / "GoldenEye Metal";
  if (!CreateFixture(journal_symlink_root, 0x55) || !WriteSentinels(journal_symlink_root)) {
    return Fail(50, "could not create the journal-symlink recovery fixture");
  }
  const pid_t journal_child = fork();
  if (journal_child == 0) {
    ge::save::testing::SetCrashAfterCheckpoint(0);
    static_cast<void>(ge::save::ResetToFresh(journal_symlink_root));
    _exit(85);
  }
  int journal_wait_status = 0;
  if (journal_child < 0 || waitpid(journal_child, &journal_wait_status, 0) != journal_child ||
      !WIFEXITED(journal_wait_status) || WEXITSTATUS(journal_wait_status) != 86) {
    return Fail(51, "could not stop a reset after its durable journal write");
  }

  const auto quarantine_base = journal_symlink_root / "Save Data Quarantine";
  std::filesystem::path recorded_quarantine;
  ec.clear();
  for (const auto& entry : std::filesystem::directory_iterator(quarantine_base, ec)) {
    if (entry.is_directory() && !entry.is_symlink()) {
      recorded_quarantine = entry.path();
      break;
    }
  }
  if (ec || recorded_quarantine.empty()) {
    return Fail(52, "could not locate the journal-recorded quarantine");
  }

  const auto real_quarantine = recorded_quarantine.string() + ".real";
  const auto external_quarantine = journal_symlink_temporary.path / "external-quarantine";
  const auto external_sentinel = external_quarantine / "must-not-change.txt";
  const std::vector<uint8_t> external_contents = {'e', 'x', 't', 'e', 'r', 'n', 'a', 'l', '-',
                                                  's', 'e', 'n', 't', 'i', 'n', 'e', 'l'};
  if (!Write(external_sentinel, external_contents)) {
    return Fail(53, "could not create the external symlink target");
  }
  std::filesystem::rename(recorded_quarantine, real_quarantine, ec);
  if (ec) {
    return Fail(54, "could not move the recorded quarantine for the symlink test");
  }
  std::filesystem::create_directory_symlink(external_quarantine, recorded_quarantine, ec);
  if (ec) {
    return Fail(55, "could not replace the recorded quarantine with a symlink");
  }

  auto rejected_recovery = ge::save::RecoverInterruptedTransaction(journal_symlink_root);
  if (rejected_recovery || rejected_recovery.recovered ||
      !std::filesystem::exists(journal_symlink_root / ".goldeneye-save-transaction") ||
      Read(external_sentinel) != external_contents ||
      Read(journal_symlink_root / kSavePayload) != std::vector<uint8_t>(512, 0x55) ||
      !SentinelsMatch(journal_symlink_root)) {
    return Fail(56, "recovery followed or modified a substituted quarantine symlink");
  }

  std::filesystem::remove(recorded_quarantine, ec);
  if (ec) {
    return Fail(57, "could not remove the substituted quarantine symlink");
  }
  std::filesystem::rename(real_quarantine, recorded_quarantine, ec);
  if (ec) {
    return Fail(58, "could not restore the real quarantine directory");
  }
  auto repaired_recovery = ge::save::RecoverInterruptedTransaction(journal_symlink_root);
  if (!repaired_recovery || !repaired_recovery.recovered ||
      !FullStateMatches(journal_symlink_root, 0x55, {1, 3, 3, 7}) ||
      !SentinelsMatch(journal_symlink_root)) {
    return Fail(59, "recovery did not resume safely after removing the symlink");
  }
#endif
  return 0;
}
