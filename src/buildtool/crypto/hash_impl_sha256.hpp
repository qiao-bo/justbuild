#ifndef INCLUDED_SRC_BUILDTOOL_CRYPTO_HASH_IMPL_SHA256_HPP
#define INCLUDED_SRC_BUILDTOOL_CRYPTO_HASH_IMPL_SHA256_HPP

#include <memory>

#include "src/buildtool/crypto/hash_impl.hpp"

[[nodiscard]] extern auto CreateHashImplSha256() -> std::unique_ptr<IHashImpl>;

#endif  // INCLUDED_SRC_BUILDTOOL_CRYPTO_HASH_IMPL_SHA256_HPP
