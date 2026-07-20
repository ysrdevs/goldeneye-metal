#pragma once

#include <rex/rex_app.h>

#include <functional>
#include <optional>

namespace rex::ui {
class WindowedAppContext;
}

namespace ge {

// Applies platform-appropriate writable user/config/cache locations.
void ConfigureLauncherPaths(rex::PathConfig& paths);

// Displays the native launcher and resumes on the UI thread only after the
// user explicitly chooses Play. The launcher also owns first-run game-data
// setup and privacy-preserving diagnostic export. Explicit noninteractive
// command-line launches may return paths synchronously.
std::optional<rex::PathConfig> PrepareLauncherPaths(const rex::PathConfig& defaults,
                                                    std::function<void(rex::PathConfig)> resume,
                                                    rex::ui::WindowedAppContext& app_context);

}  // namespace ge
