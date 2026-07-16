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

// Returns paths synchronously when valid game data is already configured.
// Otherwise displays the native first-run launcher and resumes on the UI
// thread after a successful local import or folder selection.
std::optional<rex::PathConfig> PrepareLauncherPaths(const rex::PathConfig& defaults,
                                                    std::function<void(rex::PathConfig)> resume,
                                                    rex::ui::WindowedAppContext& app_context);

}  // namespace ge
