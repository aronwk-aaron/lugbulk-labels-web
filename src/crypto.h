// Small crypto helpers built on OpenSSL EVP: AES-256-GCM for encrypting
// refresh tokens at rest, base64 for key/blob transport, and CSPRNG bytes
// for session tokens / OAuth `state`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lugbulk::crypto {

// Raises std::runtime_error (message never includes key/plaintext material)
// on any OpenSSL failure.

// n cryptographically-random bytes from OpenSSL's RNG.
std::vector<uint8_t> random_bytes(size_t n);

// Random bytes, hex-encoded — used for session tokens / OAuth state, so the
// result is URL- and cookie-safe without extra escaping.
std::string random_hex_token(size_t n_bytes = 32);

std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& b64);

// AES-256-GCM. `key` must decode to exactly 32 bytes. Output blob layout is
// [12-byte nonce][16-byte tag][ciphertext] so decrypt is self-contained
// given just the key.
std::vector<uint8_t> aes_gcm_encrypt(const std::string& key_b64, const std::string& plaintext);
std::string aes_gcm_decrypt(const std::string& key_b64, const std::vector<uint8_t>& blob);

}  // namespace lugbulk::crypto
