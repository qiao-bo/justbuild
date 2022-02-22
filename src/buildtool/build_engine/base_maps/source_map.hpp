#ifndef INCLUDED_SRC_BUILDTOOL_BUILD_ENGINE_BASE_MAPS_SOURCE_MAP_HPP
#define INCLUDED_SRC_BUILDTOOL_BUILD_ENGINE_BASE_MAPS_SOURCE_MAP_HPP

#include <unordered_set>

#include "nlohmann/json.hpp"
#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/build_engine/analysed_target/analysed_target.hpp"
#include "src/buildtool/build_engine/base_maps/directory_map.hpp"
#include "src/buildtool/build_engine/base_maps/entity_name.hpp"
#include "src/buildtool/build_engine/expression/expression.hpp"
#include "src/buildtool/multithreading/async_map_consumer.hpp"
#include "src/buildtool/multithreading/task_system.hpp"

namespace BuildMaps::Base {

using SourceTargetMap = AsyncMapConsumer<EntityName, AnalysedTargetPtr>;

auto CreateSourceTargetMap(const gsl::not_null<DirectoryEntriesMap*>& dirs,
                           std::size_t jobs = 0) -> SourceTargetMap;

}  // namespace BuildMaps::Base

#endif
