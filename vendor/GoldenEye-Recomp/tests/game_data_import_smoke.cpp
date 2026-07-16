#include "ge_game_data.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: game_data_import_smoke <LIVE/STFS package> <destination> "
                 "[cancel-after-bytes]\n";
    return 2;
  }

  const std::filesystem::path package = argv[1];
  const std::filesystem::path destination = argv[2];
  uint64_t cancel_after = argc == 4 ? std::stoull(argv[3]) : 0;
  std::atomic<uint64_t> completed{0};
  uint64_t last_printed = 0;
  std::string last_message;
  auto result = ge::game_data::ImportPackage(
      package, destination,
      [&](const ge::game_data::Progress& progress) {
        completed.store(progress.completed);
        bool message_changed = progress.message != last_message;
        if (!message_changed && progress.completed < last_printed + 8 * 1024 * 1024 &&
            progress.completed != progress.total) {
          return;
        }
        last_message = progress.message;
        last_printed = progress.completed;
        if (progress.total != 0) {
          std::cout << progress.message << " " << progress.completed << "/" << progress.total
                    << "\r" << std::flush;
        } else {
          std::cout << progress.message << "\n";
        }
      },
      [&] { return cancel_after != 0 && completed.load() >= cancel_after; });
  std::cout << "\n";
  if (!result) {
    std::cerr << result.error << "\n";
    return result.error == "Import cancelled." ? 3 : 1;
  }

  auto validation = ge::game_data::ValidateImportedDirectory(result.game_data_root);
  if (!validation.valid) {
    std::cerr << validation.error << "\n";
    return 1;
  }
  std::cout << "Imported and validated: " << result.game_data_root << "\n";
  return 0;
}
