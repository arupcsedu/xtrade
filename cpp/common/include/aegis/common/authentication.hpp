#ifndef AEGIS_COMMON_AUTHENTICATION_HPP
#define AEGIS_COMMON_AUTHENTICATION_HPP

#include "aegis/common/sha256.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace aegis::common {

inline constexpr std::size_t kEd25519PublicKeyBytes = 32U;
inline constexpr std::size_t kEd25519PrivateKeyBytes = 32U;
inline constexpr std::size_t kEd25519SignatureBytes = 64U;
inline constexpr std::size_t kHmacSha256KeyBytes = 32U;
inline constexpr std::size_t kMaximumHmacMessageBytes = 2'048U;

using Ed25519PublicKey = std::array<std::uint8_t, kEd25519PublicKeyBytes>;
using Ed25519PrivateKey = std::array<std::uint8_t, kEd25519PrivateKeyBytes>;
using Ed25519Signature = std::array<std::uint8_t, kEd25519SignatureBytes>;
using HmacSha256Key = std::array<std::uint8_t, kHmacSha256KeyBytes>;

// HMAC is used only for bounded, per-process/session capabilities. The key is
// supplied out of band and is never serialized, hashed into configuration, or
// logged. The implementation performs no allocation.
[[nodiscard]] bool hmac_sha256(const HmacSha256Key& key,
                               std::span<const std::uint8_t> message,
                               Sha256Digest& output) noexcept;

[[nodiscard]] bool constant_time_equal(std::span<const std::uint8_t> left,
                                       std::span<const std::uint8_t> right) noexcept;

// Ed25519 operations are control-path operations and may allocate inside the
// system cryptographic provider. They must not be called from an order loop.
[[nodiscard]] bool ed25519_derive_public_key(const Ed25519PrivateKey& private_key,
                                             Ed25519PublicKey& public_key) noexcept;
[[nodiscard]] bool ed25519_sign(const Ed25519PrivateKey& private_key,
                                std::span<const std::uint8_t> message,
                                Ed25519Signature& signature) noexcept;
[[nodiscard]] bool ed25519_verify(const Ed25519PublicKey& public_key,
                                  std::span<const std::uint8_t> message,
                                  const Ed25519Signature& signature) noexcept;

} // namespace aegis::common

#endif // AEGIS_COMMON_AUTHENTICATION_HPP
