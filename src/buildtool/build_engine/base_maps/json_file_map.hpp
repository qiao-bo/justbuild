#ifndef INCLUDED_SRC_BUILDTOOL_BUILD_ENGINE_BASE_MAPS_JSON_FILE_MAP_HPP
#define INCLUDED_SRC_BUILDTOOL_BUILD_ENGINE_BASE_MAPS_JSON_FILE_MAP_HPP

#include <filesystem>
#include <fstream>
#include <string>

#include "fmt/core.h"
#include "nlohmann/json.hpp"
#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/build_engine/base_maps/module_name.hpp"
#include "src/buildtool/common/repository_config.hpp"
#include "src/buildtool/multithreading/async_map_consumer.hpp"

namespace BuildMaps::Base {

using JsonFileMap = AsyncMapConsumer<ModuleName, nlohmann::json>;

// function pointer type for specifying which root to get from global config
using RootGetter = auto (RepositoryConfig::*)(std::string const&) const
                   -> FileRoot const*;

// function pointer type for specifying the file name from the global config
using FileNameGetter = auto (RepositoryConfig::*)(std::string const&) const
                       -> std::string const*;

template <RootGetter get_root, FileNameGetter get_name, bool kMandatory = true>
auto CreateJsonFileMap(std::size_t jobs) -> JsonFileMap {
    auto json_file_reader = [](auto /* unused */,
                               auto setter,
                               auto logger,
                               auto /* unused */,
                               auto const& key) {
        auto const& config = RepositoryConfig::Instance();
        auto const* root = (config.*get_root)(key.repository);
        auto const* json_file_name = (config.*get_name)(key.repository);
        if (root == nullptr or json_file_name == nullptr) {
            (*logger)(fmt::format("Cannot determine root or JSON file name for "
                                  "repository {}.",
                                  key.repository),
                      true);
            return;
        }
        auto module = std::filesystem::path{key.module}.lexically_normal();
        if (module.is_absolute() or *module.begin() == "..") {
            (*logger)(fmt::format("Modules have to live inside their "
                                  "repository, but found {}.",
                                  key.module),
                      true);
            return;
        }
        auto json_file_path = module / *json_file_name;

        if (not root->IsFile(json_file_path)) {
            if constexpr (kMandatory) {
                (*logger)(fmt::format("JSON file {} does not exist.",
                                      json_file_path.string()),
                          true);
            }
            else {
                (*setter)(nlohmann::json::object());
            }
            return;
        }

        auto const file_content = root->ReadFile(json_file_path);
        if (not file_content) {
            (*logger)(fmt::format("cannot read JSON file {}.",
                                  json_file_path.string()),
                      true);
            return;
        }
        auto json = nlohmann::json::parse(*file_content, nullptr, false);
        if (json.is_discarded()) {
            (*logger)(fmt::format("JSON file {} does not contain valid JSON.",
                                  json_file_path.string()),
                      true);
            return;
        }
        if (!json.is_object()) {
            (*logger)(fmt::format("JSON in {} is not an object.",
                                  json_file_path.string()),
                      true);
            return;
        }
        (*setter)(std::move(json));
    };
    return AsyncMapConsumer<ModuleName, nlohmann::json>{json_file_reader, jobs};
}

}  // namespace BuildMaps::Base

#endif  // INCLUDED_SRC_BUILDTOOL_BUILD_ENGINE_BASE_MAPS_JSON_FILE_MAP_HPP
