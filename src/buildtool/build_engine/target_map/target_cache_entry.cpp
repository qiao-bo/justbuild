// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/buildtool/build_engine/target_map/target_cache_entry.hpp"

#include <algorithm>
#include <exception>
#include <iterator>

#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"

auto TargetCacheEntry::FromTarget(
    AnalysedTargetPtr const& target,
    std::unordered_map<ArtifactDescription, Artifact::ObjectInfo> const&
        replacements) noexcept -> std::optional<TargetCacheEntry> {
    auto result = TargetResult{
        target->Artifacts(), target->Provides(), target->RunFiles()};
    if (auto desc = result.ReplaceNonKnownAndToJson(replacements)) {
        return TargetCacheEntry{*desc};
    }
    return std::nullopt;
}

auto TargetCacheEntry::FromJson(nlohmann::json desc) noexcept
    -> TargetCacheEntry {
    return TargetCacheEntry(std::move(desc));
}

auto TargetCacheEntry::ToResult() const noexcept
    -> std::optional<TargetResult> {
    return TargetResult::FromJson(desc_);
}

[[nodiscard]] auto ToObjectInfo(nlohmann::json const& json)
    -> Artifact::ObjectInfo {
    auto const& desc = ArtifactDescription::FromJson(json);
    // The assumption is that all artifacts mentioned in a target cache
    // entry are KNOWN to the remote side.
    gsl_ExpectsAudit(desc and desc->IsKnown());
    auto const& info = desc->ToArtifact().Info();
    gsl_ExpectsAudit(info);
    return *info;
}

[[nodiscard]] auto ScanArtifactMap(
    gsl::not_null<std::vector<Artifact::ObjectInfo>*> const& infos,
    nlohmann::json const& json) -> bool {
    if (not json.is_object()) {
        return false;
    }
    infos->reserve(infos->size() + json.size());
    std::transform(json.begin(),
                   json.end(),
                   std::back_inserter(*infos),
                   [](auto const& item) { return ToObjectInfo(item); });
    return true;
}

[[nodiscard]] auto ScanProvidesMap(
    gsl::not_null<std::vector<Artifact::ObjectInfo>*> const& infos,
    nlohmann::json const& json) -> bool {
    if (not json.is_object()) {
        return false;
    }
    auto const& nodes = json["nodes"];
    auto const& provided_artifacts = json["provided_artifacts"];
    infos->reserve(infos->size() + provided_artifacts.size());
    std::transform(
        provided_artifacts.begin(),
        provided_artifacts.end(),
        std::back_inserter(*infos),
        [&nodes](auto const& item) {
            return ToObjectInfo(nodes[item.template get<std::string>()]);
        });
    return true;
}

auto TargetCacheEntry::ToArtifacts(
    gsl::not_null<std::vector<Artifact::ObjectInfo>*> const& infos)
    const noexcept -> bool {
    try {
        if (ScanArtifactMap(infos, desc_["artifacts"]) and
            ScanArtifactMap(infos, desc_["runfiles"]) and
            ScanProvidesMap(infos, desc_["provides"])) {
            return true;
        }
    } catch (std::exception const& ex) {
        Logger::Log(
            LogLevel::Error,
            "Scanning target cache entry for artifacts failed with:\n{}",
            ex.what());
    }
    return false;
}
