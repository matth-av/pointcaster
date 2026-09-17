#include "workspace_config.h"
#include <algorithm>
#include <exception>
#include <iostream>
#include <logger/logger.h>
#include <rfl/yaml.hpp>

namespace pc {

// the workspace configuration gets serialized as a named
// top-level element
//
// every read needs rfl::DefaultIfMissing, it's what lets configuration
// structs hold plain members: a missing field keeps its in-class initialiser
using WorkspaceFile =
    rfl::NamedTuple<rfl::Field<"workspace", WorkspaceConfiguration>>;

bool load_workspace_from_file(WorkspaceConfiguration &config,
                              const std::string &file_path,
                              bool load_malformed) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    pc::logger()->error("Could not open '{}'", file_path);
    config = WorkspaceConfiguration{};
    return false;
  }
  const std::string yaml_string{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};

  auto read_result =
      rfl::yaml::read<WorkspaceFile, rfl::AddTagsToVariants,
                      rfl::SnakeCaseToCamelCase, rfl::DefaultIfMissing,
                      rfl::NoExtraFields>(yaml_string);

  if (!read_result && load_malformed) {
    // still attempt to load the workspace file, just log the errors
    const std::string error_msg = read_result.error().what();
    read_result =
        rfl::yaml::read<WorkspaceFile, rfl::AddTagsToVariants,
                        rfl::SnakeCaseToCamelCase, rfl::DefaultIfMissing>(
            yaml_string);
    if (read_result) {
      pc::logger()->error("Loaded '{}' with error:", file_path);
      pc::logger()->error(error_msg);
    }
  }

  if (!read_result) {
    pc::logger()->error("Failed to parse '{}': {}", file_path,
                        read_result.error().what());
    config = WorkspaceConfiguration{};
    return false;
  }

  config = std::move(read_result.value().get<"workspace">());
  pc::logger()->info("Loaded configuration from '{}'", file_path);
  pc::logger()->trace(
      "Loaded Workspace:\n{}",
      rfl::yaml::write<rfl::AddTagsToVariants, rfl::SnakeCaseToCamelCase>(
          WorkspaceFile(config)));
  return true;
}

void save_workspace_to_file(const WorkspaceConfiguration &config,
                            const std::string &file_path) {
  try {
    const auto yaml_string =
        rfl::yaml::write<rfl::AddTagsToVariants, rfl::SnakeCaseToCamelCase>(
            WorkspaceFile(config));
    std::ofstream(file_path) << yaml_string;
    pc::logger()->info("Saved workspace file to '{}'", file_path);
  } catch (const std::exception &e) {
    pc::logger()->error("Failed to save '{}': {}", file_path, e.what());
  }
}

std::optional<std::reference_wrapper<SessionConfiguration>>
session_config_from_workspace(WorkspaceConfiguration &config,
                              std::string_view session_id) {
  auto it = std::find_if(
      config.sessions.begin(), config.sessions.end(),
      [session_id](auto &session) { return session.id == session_id; });
  if (it == config.sessions.end()) return std::nullopt;
  return std::ref(*it);
}

} // namespace pc