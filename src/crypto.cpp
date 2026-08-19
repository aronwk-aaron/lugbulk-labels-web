#include "crypto.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace lugbulk::crypto {

namespace {

constexpr size_t kKeyLen = 32;    // AES-256
constexpr size_t kNonceLen = 12;  // 96-bit GCM nonce (standard)
constexpr size_t kTagLen = 16;    // 128-bit GCM auth tag

[[noreturn]] void throw_openssl_error(const char* what) {
    // Deliberately does not include ERR_reason_error_string() contents in
    // case a buffer/key pointer ever ends up embedded in an OpenSSL error
    // string; the fixed message is enough to diagnose from logs.
    ERR_clear_error();
    throw std::runtime_error(std::string("crypto error: ") + what);
}

std::string hex_encode(const std::vector<uint8_t>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t b : data) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

}  // namespace

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> buf(n);
    if (n > 0 && RAND_bytes(buf.data(), static_cast<int>(n)) != 1) {
        throw_openssl_error("RAND_bytes failed");
    }
    return buf;
}

std::string random_hex_token(size_t n_bytes) {
    return hex_encode(random_bytes(n_bytes));
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    // EVP_EncodeBlock has no newlines and is simplest for a single-shot blob.
    std::vector<uint8_t> out(4 * ((data.size() + 2) / 3) + 1);
    int len = EVP_EncodeBlock(out.data(), data.data(), static_cast<int>(data.size()));
    return std::string(reinterpret_cast<char*>(out.data()), static_cast<size_t>(len));
}

std::vector<uint8_t> base64_decode(const std::string& b64) {
    if (b64.empty()) return {};
    std::vector<uint8_t> out(3 * ((b64.size() + 3) / 4) + 1);
    int len = EVP_DecodeBlock(out.data(), reinterpret_cast<const uint8_t*>(b64.data()),
                               static_cast<int>(b64.size()));
    if (len < 0) throw_openssl_error("base64 decode failed");
    // EVP_DecodeBlock doesn't account for '=' padding; trim it off.
    size_t pad = 0;
    if (b64.size() >= 1 && b64[b64.size() - 1] == '=') pad++;
    if (b64.size() >= 2 && b64[b64.size() - 2] == '=') pad++;
    out.resize(static_cast<size_t>(len) - pad);
    return out;
}

std::vector<uint8_t> aes_gcm_encrypt(const std::string& key_b64, const std::string& plaintext) {
    std::vector<uint8_t> key = base64_decode(key_b64);
    if (key.size() != kKeyLen) {
        throw std::runtime_error("crypto error: TOKEN_ENCRYPTION_KEY must decode to 32 bytes");
    }
    std::vector<uint8_t> nonce = random_bytes(kNonceLen);

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl_error("EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw_openssl_error("EncryptInit (cipher) failed");
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen),
                             nullptr) != 1)
        throw_openssl_error("set IV length failed");
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
        throw_openssl_error("EncryptInit (key/iv) failed");

    std::vector<uint8_t> ciphertext(plaintext.size());
    int out_len = 0;
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &out_len,
                               reinterpret_cast<const uint8_t*>(plaintext.data()),
                               static_cast<int>(plaintext.size())) != 1)
            throw_openssl_error("EncryptUpdate failed");
    }
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + out_len, &final_len) != 1)
        throw_openssl_error("EncryptFinal failed");
    ciphertext.resize(static_cast<size_t>(out_len + final_len));

    std::vector<uint8_t> tag(kTagLen);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen),
                             tag.data()) != 1)
        throw_openssl_error("get tag failed");

    std::vector<uint8_t> blob;
    blob.reserve(nonce.size() + tag.size() + ciphertext.size());
    blob.insert(blob.end(), nonce.begin(), nonce.end());
    blob.insert(blob.end(), tag.begin(), tag.end());
    blob.insert(blob.end(), ciphertext.begin(), ciphertext.end());

    // Best-effort scrub of the plaintext copy we made (ciphertext buffer is
    // already the encrypted form; the caller's std::string is theirs to
    // manage, we only had our own local buffers here).
    OPENSSL_cleanse(key.data(), key.size());
    return blob;
}

std::string aes_gcm_decrypt(const std::string& key_b64, const std::vector<uint8_t>& blob) {
    std::vector<uint8_t> key = base64_decode(key_b64);
    if (key.size() != kKeyLen) {
        throw std::runtime_error("crypto error: TOKEN_ENCRYPTION_KEY must decode to 32 bytes");
    }
    if (blob.size() < kNonceLen + kTagLen) {
        throw std::runtime_error("crypto error: ciphertext blob too short");
    }
    const uint8_t* nonce = blob.data();
    const uint8_t* tag = blob.data() + kNonceLen;
    const uint8_t* ct = blob.data() + kNonceLen + kTagLen;
    size_t ct_len = blob.size() - kNonceLen - kTagLen;

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl_error("EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw_openssl_error("DecryptInit (cipher) failed");
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen),
                             nullptr) != 1)
        throw_openssl_error("set IV length failed");
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce) != 1)
        throw_openssl_error("DecryptInit (key/iv) failed");

    std::vector<uint8_t> plaintext(ct_len);
    int out_len = 0;
    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len, ct,
                               static_cast<int>(ct_len)) != 1)
            throw_openssl_error("DecryptUpdate failed");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                             const_cast<uint8_t*>(tag)) != 1)
        throw_openssl_error("set tag failed");

    int final_len = 0;
    int ok = EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + out_len, &final_len);
    OPENSSL_cleanse(key.data(), key.size());
    if (ok != 1) {
        // Auth tag mismatch: tampered or wrong-key ciphertext. Never leak
        // which part failed.
        throw std::runtime_error("crypto error: decryption failed (bad key or tampered data)");
    }
    plaintext.resize(static_cast<size_t>(out_len + final_len));
    std::string result(reinterpret_cast<char*>(plaintext.data()), plaintext.size());
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    return result;
}

}  // namespace lugbulk::crypto
