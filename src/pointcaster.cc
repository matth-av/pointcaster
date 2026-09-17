#include <QCoreApplication>
#include <QGuiApplication>
#include <app_settings/app_settings.h>
#include <core/logger/logger.h>
#include <core/profiling/profiler.h>
#include <optional>
#include <pointcaster/point_cloud.h>
#include <pointcaster/task_pool.h>
#include <print>
#include <tracy/Tracy.hpp>
#include <ui/initialisation.h>
#include <ui/models/workspace_model.h>
#include <workspace/workspace.h>

using namespace pc;

int main(int argc, char *argv[]) {
  std::println("This is pointcaster 0.2.14");

  auto *app_settings = AppSettings::instance();

  if (app_settings->logToFile()) {
    pc::enable_file_logging("pointcaster-qt");
  }
  const auto toggle_file_logging = [&] {
    if (app_settings->logToFile()) {
      pc::enable_file_logging("pointcaster-qt");
    } else {
      pc::disable_file_logging();
    }
  };
  QObject::connect(app_settings, &pc::AppSettings::logToFileChanged,
                   app_settings, toggle_file_logging);

  pc::set_log_level(app_settings->spdlogLogLevel());
  QObject::connect(app_settings, &pc::AppSettings::logLevelChanged,
                   app_settings,
                   [&] { pc::set_log_level(app_settings->spdlogLogLevel()); });

  // the preference is a diameter in millimetres, the radius a point without a
  // scale of its own draws at is half of it
  const auto apply_point_size_pref = [&] {
    pc::default_point_scale_millimetres().store(app_settings->pointSize() *
                                                0.5f);
  };
  apply_point_size_pref();
  QObject::connect(app_settings, &pc::AppSettings::pointSizeChanged,
                   app_settings, apply_point_size_pref);

  const auto apply_worker_thread_prefs = [&] {
    pc::set_task_pool_size(
        static_cast<std::size_t>(app_settings->workerThreads()));
  };
  apply_worker_thread_prefs();
  QObject::connect(app_settings, &pc::AppSettings::workerThreadsChanged,
                   app_settings, apply_worker_thread_prefs);

  pc::logger()->trace("Task thread pool using {} threads",
                      pc::task_pool().get_thread_count());

  // tracy profiling is enabled or disabled for the entire application run
  if (app_settings->enableTracyProfiling()) {
    pc::profiling::start_profiler();
  }

  pc::logger()->trace("Starting QGuiApplication...");
  QGuiApplication app(argc, argv);

  pc::logger()->trace("Initialising Workspace...");

  WorkspaceConfiguration workspace_config;

  std::optional<std::string> loaded_workspace_path;
  if (app_settings->restoreLastWorkspace() &&
      !app_settings->lastWorkspacePath().isEmpty()) {
    const auto last_workspace_path =
        app_settings->lastWorkspacePath().toStdString();
    if (load_workspace_from_file(workspace_config, last_workspace_path,
                                 app_settings->loadMalformedWorkspaces())) {
      loaded_workspace_path = last_workspace_path;
    };
  }

  // create a pointcaster workspace using this initial configuration, either
  // auto-loaded or empty
  Workspace workspace(workspace_config);

  pc::logger()->trace("Initialised Workspace");
  pc::logger()->trace("Initialising Workspace model");

  pc::ui::WorkspaceModel workspace_model{&workspace, &app};

  pc::logger()->trace("Initialised Workspace model");

  pc::logger()->trace("Starting QQmlApplicationEngine...");
  auto *gui_engine =
      pc::ui::initialise(&app, &workspace_model, loaded_workspace_path);

  pc::logger()->trace("Loading main window");

  pc::ui::load_main_window(&workspace, &app, gui_engine);

  pc::logger()->trace("Application load complete");

  return app.exec();
}
