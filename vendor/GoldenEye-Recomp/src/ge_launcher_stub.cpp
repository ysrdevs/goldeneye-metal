#include "ge_launcher.h"

namespace ge {

void ConfigureLauncherPaths(rex::PathConfig& paths) {
  (void)paths;
}

std::optional<rex::PathConfig> PrepareLauncherPaths(const rex::PathConfig& defaults,
                                                    std::function<void(rex::PathConfig)> resume,
                                                    rex::ui::WindowedAppContext& app_context) {
  (void)resume;
  (void)app_context;
  return defaults;
}

}  // namespace ge
