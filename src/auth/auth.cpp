#include "mcp/auth/auth.hpp"
#include "mcp/core/errors.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mcp::auth {

// ═══════════════════════════════════════════════════════════════════════════
//  Utility helpers (shared by all backends)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr char kHexChars[] = "0123456789abcdef";

std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHexChars[(data[i] >> 4) & 0x0F];
        out += kHexChars[data[i] & 0x0F];
    }
    return out;
}

uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw McpError("Invalid hex character in key");
}

std::vector<uint8_t> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw McpError("Hex string must have even length");
    }
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(
            (hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
    }
    return out;
}

// Base64 encoding (RFC 4648)
constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kBase64Chars[n & 0x3F] : '=';
    }
    return out;
}

int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64_decode(const std::string& in) {
    std::vector<uint8_t> out;
    out.reserve((in.size() / 4) * 3);

    uint32_t buf = 0;
    int bits = 0;

    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int val = base64_decode_char(c);
        if (val < 0) {
            throw McpError("Invalid base64 character in auth token");
        }
        buf = (buf << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  AuthKey
// ═══════════════════════════════════════════════════════════════════════════

AuthKey::AuthKey() : key_(kKeySize, 0) {}

AuthKey::~AuthKey() { secure_zero(); }

AuthKey::AuthKey(const AuthKey& other) : key_(other.key_) {}

AuthKey& AuthKey::operator=(const AuthKey& other) {
    if (this != &other) {
        secure_zero();
        key_ = other.key_;
    }
    return *this;
}

AuthKey::AuthKey(AuthKey&& other) noexcept : key_(std::move(other.key_)) {
    other.key_.resize(kKeySize, 0);
}

AuthKey& AuthKey::operator=(AuthKey&& other) noexcept {
    if (this != &other) {
        secure_zero();
        key_ = std::move(other.key_);
        other.key_.resize(kKeySize, 0);
    }
    return *this;
}

bool AuthKey::is_valid() const noexcept {
    for (auto b : key_) {
        if (b != 0) return true;
    }
    return false;
}

void AuthKey::secure_zero() {
    // volatile to prevent compiler from optimizing away the zeroing
    volatile uint8_t* p = key_.data();
    for (size_t i = 0; i < key_.size(); ++i) {
        p[i] = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Key loading
// ═══════════════════════════════════════════════════════════════════════════

AuthKey key_from_hex(const std::string& hex) {
    if (hex.size() != kKeyHexSize) {
        throw McpError("Auth key must be exactly 64 hex characters (256 bits)");
    }
    auto bytes = from_hex(hex);
    AuthKey key;
    std::memcpy(key.data(), bytes.data(), kKeySize);
    return key;
}

std::string key_to_hex(const AuthKey& key) {
    return to_hex(key.data(), key.size());
}

AuthKey load_key(const KeyConfig& config) {
    switch (config.source) {
        case KeySource::EnvVariable: {
#ifdef _WIN32
            char* env_val = nullptr;
            size_t env_len = 0;
            _dupenv_s(&env_val, &env_len, config.env_var_name.c_str());
            if (!env_val || env_len == 0) {
                free(env_val);
                throw McpError("Auth key environment variable '" +
                               config.env_var_name + "' is not set");
            }
            std::string hex_str(env_val);
            free(env_val);
#else
            const char* raw_val = std::getenv(config.env_var_name.c_str());
            if (!raw_val || std::strlen(raw_val) == 0) {
                throw McpError("Auth key environment variable '" +
                               config.env_var_name + "' is not set");
            }
            std::string hex_str(raw_val);
#endif
            return key_from_hex(hex_str);
        }

        case KeySource::File: {
            if (config.file_path.empty()) {
                throw McpError("Auth key file path is empty");
            }
            std::ifstream f(config.file_path, std::ios::binary);
            if (!f) {
                throw McpError("Cannot open auth key file: " +
                               config.file_path);
            }
            // Read entire file
            std::string content(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

            // Trim whitespace
            while (!content.empty() &&
                   (content.back() == '\n' || content.back() == '\r' ||
                    content.back() == ' ')) {
                content.pop_back();
            }

            if (content.size() == kKeySize) {
                // Raw 32 bytes
                AuthKey key;
                std::memcpy(key.data(), content.data(), kKeySize);
                return key;
            }
            if (content.size() == kKeyHexSize) {
                // 64 hex chars
                return key_from_hex(content);
            }
            throw McpError(
                "Auth key file must contain 32 raw bytes or 64 hex "
                "characters, got " + std::to_string(content.size()) +
                " bytes");
        }

        case KeySource::Raw: {
            if (config.raw_key.size() != kKeySize) {
                throw McpError(
                    "Raw auth key must be exactly 32 bytes, got " +
                    std::to_string(config.raw_key.size()));
            }
            AuthKey key;
            std::memcpy(key.data(), config.raw_key.data(), kKeySize);
            return key;
        }
    }
    throw McpError("Unknown key source");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Wire format
// ═══════════════════════════════════════════════════════════════════════════

std::string token_to_wire(const EncryptedToken& token) {
    // [IV(12) || ciphertext(N) || tag(16)]
    std::vector<uint8_t> blob;
    blob.reserve(token.iv.size() + token.ciphertext.size() + token.tag.size());
    blob.insert(blob.end(), token.iv.begin(), token.iv.end());
    blob.insert(blob.end(), token.ciphertext.begin(), token.ciphertext.end());
    blob.insert(blob.end(), token.tag.begin(), token.tag.end());
    return base64_encode(blob.data(), blob.size());
}

std::optional<EncryptedToken> token_from_wire(const std::string& wire) {
    std::vector<uint8_t> blob;
    try {
        blob = base64_decode(wire);
    } catch (...) {
        return std::nullopt;
    }

    // Minimum: IV(12) + tag(16) = 28 (ciphertext can be 0 bytes for empty plaintext)
    if (blob.size() < kIvSize + kTagSize) {
        return std::nullopt;
    }

    EncryptedToken token;
    token.iv.assign(blob.begin(), blob.begin() + static_cast<ptrdiff_t>(kIvSize));
    token.ciphertext.assign(
        blob.begin() + static_cast<ptrdiff_t>(kIvSize),
        blob.end() - static_cast<ptrdiff_t>(kTagSize));
    token.tag.assign(blob.end() - static_cast<ptrdiff_t>(kTagSize), blob.end());
    return token;
}

// ═══════════════════════════════════════════════════════════════════════════
//  AuthProvider
// ═══════════════════════════════════════════════════════════════════════════

AuthProvider::AuthProvider(KeyConfig config) {
    try {
        key_ = load_key(config);
        enabled_ = key_.is_valid();
    } catch (const McpError&) {
        enabled_ = false;
    }
}

AuthProvider::~AuthProvider() = default;

bool AuthProvider::is_enabled() const noexcept {
    return enabled_;
}

std::string AuthProvider::encrypt_for_header(const std::string& token) {
    if (!enabled_) {
        throw McpError("Auth is not enabled (key not loaded)");
    }
    auto encrypted = encrypt_token(key_, token);
    return token_to_wire(encrypted);
}

std::optional<std::string> AuthProvider::validate_header(
    const std::string& header_value) {
    if (!enabled_) return std::nullopt;

    auto parsed = token_from_wire(header_value);
    if (!parsed) return std::nullopt;

    return decrypt_token(key_, *parsed);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Platform-specific AES-256-GCM + CSPRNG
// ═══════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)
// ─── Windows: BCrypt (CNG) ──────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {

void fill_random(uint8_t* buf, size_t len) {
    NTSTATUS status = BCryptGenRandom(
        nullptr, buf, static_cast<ULONG>(len),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(status)) {
        throw mcp::McpError("BCryptGenRandom failed");
    }
}

} // namespace

AuthKey generate_key() {
    AuthKey key;
    fill_random(key.data(), kKeySize);
    return key;
}

EncryptedToken encrypt_token(const AuthKey& key,
                              const std::string& plaintext) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                         nullptr, 0);
    if (!NT_SUCCESS(status)) {
        throw McpError("BCryptOpenAlgorithmProvider failed for AES");
    }

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw McpError("Failed to set GCM chaining mode");
    }

    status = BCryptGenerateSymmetricKey(
        hAlg, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw McpError("BCryptGenerateSymmetricKey failed");
    }

    EncryptedToken token;
    token.iv.resize(kIvSize);
    fill_random(token.iv.data(), kIvSize);
    token.tag.resize(kTagSize);
    // BCrypt needs non-null buffers even for empty plaintext
    uint8_t dummy = 0;
    token.ciphertext.resize(plaintext.size());

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = token.iv.data();
    authInfo.cbNonce = static_cast<ULONG>(kIvSize);
    authInfo.pbTag   = token.tag.data();
    authInfo.cbTag   = static_cast<ULONG>(kTagSize);

    PUCHAR pbInput = plaintext.empty()
        ? &dummy
        : reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data()));
    PUCHAR pbOutput = token.ciphertext.empty()
        ? &dummy
        : token.ciphertext.data();

    ULONG cbResult = 0;
    status = BCryptEncrypt(
        hKey,
        pbInput,
        static_cast<ULONG>(plaintext.size()),
        &authInfo,
        nullptr, 0,
        pbOutput,
        static_cast<ULONG>(plaintext.size()),
        &cbResult, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!NT_SUCCESS(status)) {
        throw McpError("AES-256-GCM encryption failed");
    }

    token.ciphertext.resize(cbResult);
    return token;
}

std::optional<std::string> decrypt_token(const AuthKey& key,
                                          const EncryptedToken& token) {
    if (token.iv.size() != kIvSize || token.tag.size() != kTagSize) {
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                         nullptr, 0);
    if (!NT_SUCCESS(status)) return std::nullopt;

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::nullopt;
    }

    status = BCryptGenerateSymmetricKey(
        hAlg, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::nullopt;
    }

    uint8_t dummy = 0;
    std::vector<uint8_t> plaintext(token.ciphertext.size());

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(token.iv.data());
    authInfo.cbNonce = static_cast<ULONG>(kIvSize);
    authInfo.pbTag   = const_cast<PUCHAR>(token.tag.data());
    authInfo.cbTag   = static_cast<ULONG>(kTagSize);

    PUCHAR pbInput = token.ciphertext.empty()
        ? &dummy
        : const_cast<PUCHAR>(token.ciphertext.data());
    PUCHAR pbOutput = plaintext.empty()
        ? &dummy
        : plaintext.data();

    ULONG cbResult = 0;
    status = BCryptDecrypt(
        hKey,
        pbInput,
        static_cast<ULONG>(token.ciphertext.size()),
        &authInfo,
        nullptr, 0,
        pbOutput,
        static_cast<ULONG>(plaintext.size()),
        &cbResult, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!NT_SUCCESS(status)) {
        // Authentication failed (tampered data, wrong key, etc.)
        return std::nullopt;
    }

    return std::string(reinterpret_cast<char*>(plaintext.data()), cbResult);
}

#else
// ─── Unix: OpenSSL libcrypto ────────────────────────────────────────────────

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

void fill_random(uint8_t* buf, size_t len) {
    if (RAND_bytes(buf, static_cast<int>(len)) != 1) {
        throw mcp::McpError("RAND_bytes failed");
    }
}

} // namespace

AuthKey generate_key() {
    AuthKey key;
    fill_random(key.data(), kKeySize);
    return key;
}

EncryptedToken encrypt_token(const AuthKey& key,
                              const std::string& plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw McpError("EVP_CIPHER_CTX_new failed");

    EncryptedToken token;
    token.iv.resize(kIvSize);
    fill_random(token.iv.data(), kIvSize);
    token.ciphertext.resize(plaintext.size() + 16);  // GCM may pad
    token.tag.resize(kTagSize);

    int ok = 1;
    int len = 0;
    int total = 0;

    ok = ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                    static_cast<int>(kIvSize), nullptr);
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), token.iv.data());

    ok = ok && EVP_EncryptUpdate(ctx, token.ciphertext.data(), &len,
        reinterpret_cast<const uint8_t*>(plaintext.data()),
        static_cast<int>(plaintext.size()));
    total = len;

    ok = ok && EVP_EncryptFinal_ex(ctx, token.ciphertext.data() + total, &len);
    total += len;

    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                    static_cast<int>(kTagSize),
                                    token.tag.data());

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        throw McpError("AES-256-GCM encryption failed");
    }

    token.ciphertext.resize(static_cast<size_t>(total));
    return token;
}

std::optional<std::string> decrypt_token(const AuthKey& key,
                                          const EncryptedToken& token) {
    if (token.iv.size() != kIvSize || token.tag.size() != kTagSize) {
        return std::nullopt;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    int ok = 1;
    int len = 0;
    int total = 0;

    std::vector<uint8_t> plaintext(token.ciphertext.size() + 16);

    ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                    static_cast<int>(kIvSize), nullptr);
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), token.iv.data());

    ok = ok && EVP_DecryptUpdate(ctx, plaintext.data(), &len,
        token.ciphertext.data(),
        static_cast<int>(token.ciphertext.size()));
    total = len;

    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
        static_cast<int>(kTagSize),
        const_cast<uint8_t*>(token.tag.data()));

    int final_ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + total, &len);
    total += len;

    EVP_CIPHER_CTX_free(ctx);

    if (!ok || final_ok <= 0) {
        // GCM authentication failed — tampered or wrong key
        return std::nullopt;
    }

    return std::string(reinterpret_cast<char*>(plaintext.data()),
                       static_cast<size_t>(total));
}

#endif

} // namespace mcp::auth
