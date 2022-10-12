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

#ifndef INCLUDED_SRC_BUILDTOOL_EXECUTION_API_REMOTE_BAZEL_BAZEL_CLIENT_COMMON_HPP
#define INCLUDED_SRC_BUILDTOOL_EXECUTION_API_REMOTE_BAZEL_BAZEL_CLIENT_COMMON_HPP

/// \file bazel_client_common.hpp
/// \brief Common types and functions required by client implementations.

#include <sstream>
#include <string>

#include "grpcpp/grpcpp.h"
#include "src/buildtool/common/bazel_types.hpp"
#include "src/buildtool/execution_api/bazel_msg/bazel_common.hpp"
#include "src/buildtool/execution_api/remote/config.hpp"
#include "src/buildtool/logging/logger.hpp"

[[maybe_unused]] [[nodiscard]] static inline auto CreateChannelWithCredentials(
    std::string const& server,
    Port port,
    std::string const& user = "",
    [[maybe_unused]] std::string const& pwd = "") noexcept {
    std::shared_ptr<grpc::ChannelCredentials> cred;
    std::string address = server + ':' + std::to_string(port);
    if (user.empty()) {
        cred = grpc::InsecureChannelCredentials();
    }
    else {
        // TODO(oreiche): set up authentication credentials
    }
    return grpc::CreateChannel(address, cred);
}

[[maybe_unused]] static inline void LogStatus(Logger const* logger,
                                              LogLevel level,
                                              grpc::Status const& s) noexcept {
    if (logger == nullptr) {
        Logger::Log(level, "{}: {}", s.error_code(), s.error_message());
    }
    else {
        logger->Emit(level, "{}: {}", s.error_code(), s.error_message());
    }
}

[[maybe_unused]] static inline void LogStatus(
    Logger const* logger,
    LogLevel level,
    google::rpc::Status const& s) noexcept {
    if (logger == nullptr) {
        Logger::Log(level, "{}: {}", s.code(), s.message());
    }
    else {
        logger->Emit(level, "{}: {}", s.code(), s.message());
    }
}

#endif  // INCLUDED_SRC_BUILDTOOL_EXECUTION_API_REMOTE_BAZEL_BAZEL_CLIENT_COMMON_HPP
