#include "aegis/common/authentication.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace aegis::common {
namespace {

inline constexpr std::size_t kSha256BlockBytes = 64U;

using PkeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdContextPointer = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

} // namespace

bool hmac_sha256(const HmacSha256Key& key, const std::span<const std::uint8_t> message,
                 Sha256Digest& output) noexcept {
  if (message.size() > kMaximumHmacMessageBytes) {
    output.fill(0U);
    return false;
  }
  std::array<std::uint8_t, kSha256BlockBytes> inner_key{};
  std::array<std::uint8_t, kSha256BlockBytes> outer_key{};
  for (std::size_t index = 0U; index < key.size(); ++index) {
    inner_key[index] = static_cast<std::uint8_t>(key[index] ^ 0x36U);
    outer_key[index] = static_cast<std::uint8_t>(key[index] ^ 0x5CU);
  }
  for (std::size_t index = key.size(); index < kSha256BlockBytes; ++index) {
    inner_key[index] = 0x36U;
    outer_key[index] = 0x5CU;
  }

  std::array<std::uint8_t, kSha256BlockBytes + kMaximumHmacMessageBytes> inner_input{};
  std::ranges::copy(inner_key, inner_input.begin());
  std::ranges::copy(message, inner_input.begin() + kSha256BlockBytes);
  const auto inner_digest = sha256(std::span<const std::uint8_t>(
      inner_input.data(), kSha256BlockBytes + message.size()));

  std::array<std::uint8_t, kSha256BlockBytes + 32U> outer_input{};
  std::ranges::copy(outer_key, outer_input.begin());
  std::ranges::copy(inner_digest, outer_input.begin() + kSha256BlockBytes);
  output = sha256(outer_input);

  std::ranges::fill(inner_key, 0U);
  std::ranges::fill(outer_key, 0U);
  std::ranges::fill(inner_input, 0U);
  std::ranges::fill(outer_input, 0U);
  return true;
}

bool constant_time_equal(const std::span<const std::uint8_t> left,
                         const std::span<const std::uint8_t> right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  std::uint8_t difference{};
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
  }
  return difference == 0U;
}

bool ed25519_derive_public_key(const Ed25519PrivateKey& private_key,
                               Ed25519PublicKey& public_key) noexcept {
  const PkeyPointer key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                     private_key.data(),
                                                     private_key.size()),
                        &EVP_PKEY_free);
  if (!key) {
    public_key.fill(0U);
    return false;
  }
  std::size_t bytes_written = public_key.size();
  if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &bytes_written) != 1 ||
      bytes_written != public_key.size()) {
    public_key.fill(0U);
    return false;
  }
  return true;
}

bool ed25519_sign(const Ed25519PrivateKey& private_key,
                  const std::span<const std::uint8_t> message,
                  Ed25519Signature& signature) noexcept {
  const PkeyPointer key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                     private_key.data(),
                                                     private_key.size()),
                        &EVP_PKEY_free);
  const MdContextPointer context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!key || !context ||
      EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
    signature.fill(0U);
    return false;
  }
  std::size_t signature_bytes = signature.size();
  if (EVP_DigestSign(context.get(), signature.data(), &signature_bytes, message.data(),
                     message.size()) != 1 ||
      signature_bytes != signature.size()) {
    signature.fill(0U);
    return false;
  }
  return true;
}

bool ed25519_verify(const Ed25519PublicKey& public_key,
                    const std::span<const std::uint8_t> message,
                    const Ed25519Signature& signature) noexcept {
  const PkeyPointer key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                    public_key.data(),
                                                    public_key.size()),
                        &EVP_PKEY_free);
  const MdContextPointer context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  return key && context &&
         EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) ==
             1 &&
         EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                          message.data(), message.size()) == 1;
}

} // namespace aegis::common
