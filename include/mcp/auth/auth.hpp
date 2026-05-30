#pragma once

/// MCP Authentication Module
/// ─────────────────────────────────────────────────────────────────────────────
/// Provides configurable symmetric-key authentication for MCP transports.
///
/// Design:
///   1. Users generate a 256-bit key (via keygen utility or their own method).
///   2. Client encrypts an auth token (e.g., API key, session ID) with AES-256-GCM.
///   3. Encrypted token is sent as an HTTP header (Mcp-Auth-Token).
///   4. Server decrypts at the transport layer, before any JSON-RPC dispatch.
///
/// Key provisioning is flexible -- choose one:
///   • Environment variable  (MCP_AUTH_KEY, hex-encoded)
///   • File path             (raw 32 bytes or 64 hex chars)
///   • Programmatic          (pass raw bytes directly)
///
/// Crypto backends:
///   • Windows: BCrypt (CNG) — FIPS 140-2 certified, zero external deps
///   • Unix:    OpenSSL libcrypto — ubiquitous, hardware-accelerated
///
/// Algorithm: AES-256-GCM (authenticated encryption with associated data)
///   • 256-bit key, 96-bit IV (random per encryption), 128-bit auth tag
///   • Provides confidentiality + integrity + authenticity in one pass

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp::auth {

// ── Constants ───────────────────────────────────────────────────────────────

inline constexpr size_t kKeySize    = 32;  // AES-256: 32 bytes
inline constexpr size_t kIvSize     = 12;  // GCM recommended: 96 bits
inline constexpr size_t kTagSize    = 16;  // GCM auth tag: 128 bits
inline constexpr size_t kKeyHexSize = 64;  // Hex-encoded key length

// ── Key representation ──────────────────────────────────────────────────────

/// A 256-bit symmetric key with secure memory handling.
/// On destruction, key material is zeroed.
class AuthKey {
public:
    AuthKey();
    ~AuthKey();

    AuthKey(const AuthKey&);
    AuthKey& operator=(const AuthKey&);
    AuthKey(AuthKey&&) noexcept;
    AuthKey& operator=(AuthKey&&) noexcept;

    /// Access raw key bytes (exactly kKeySize).
    [[nodiscard]] const uint8_t* data() const noexcept { return key_.data(); }
    [[nodiscard]] uint8_t*       data() noexcept       { return key_.data(); }
    [[nodiscard]] size_t         size() const noexcept  { return kKeySize; }

    /// True if the key has been populated (not all zeros).
    [[nodiscard]] bool is_valid() const noexcept;

private:
    std::vector<uint8_t> key_;  // Always kKeySize bytes

    void secure_zero();
};

// ── Key providers ───────────────────────────────────────────────────────────

/// Strategy for loading the encryption key. Users pick whichever suits
/// their deployment model.
enum class KeySource {
    EnvVariable,   // Read from MCP_AUTH_KEY environment variable (hex)
    File,          // Read from a file path (32 raw bytes or 64 hex chars)
    Raw            // Provided directly as raw bytes
};

/// Configuration for key loading.
struct KeyConfig {
    KeySource source = KeySource::EnvVariable;

    /// For KeySource::EnvVariable — override the env var name (default: "MCP_AUTH_KEY")
    std::string env_var_name = "MCP_AUTH_KEY";

    /// For KeySource::File — path to key file
    std::string file_path;

    /// For KeySource::Raw — raw key bytes (must be exactly kKeySize)
    std::vector<uint8_t> raw_key;
};

/// Load a key according to the given configuration.
/// Throws McpError on failure (missing env var, bad file, wrong size, etc.)
[[nodiscard]] AuthKey load_key(const KeyConfig& config);

/// Generate a new random 256-bit key (uses platform CSPRNG).
[[nodiscard]] AuthKey generate_key();

/// Convert a key to hex string (for storage/display).
[[nodiscard]] std::string key_to_hex(const AuthKey& key);

/// Parse a hex-encoded key string. Throws on invalid input.
[[nodiscard]] AuthKey key_from_hex(const std::string& hex);

// ── Encryption / Decryption ─────────────────────────────────────────────────

/// Encrypted token payload.
/// Wire format: [12-byte IV] [ciphertext] [16-byte GCM tag]
/// The entire blob is base64-encoded for HTTP header transport.
struct EncryptedToken {
    std::vector<uint8_t> iv;          // 12 bytes
    std::vector<uint8_t> ciphertext;  // Variable length
    std::vector<uint8_t> tag;         // 16 bytes
};

/// Encrypt a plaintext token with AES-256-GCM.
/// Returns the encrypted token structure.
/// Throws McpError on crypto failure.
[[nodiscard]] EncryptedToken encrypt_token(const AuthKey& key,
                                           const std::string& plaintext);

/// Decrypt an encrypted token with AES-256-GCM.
/// Returns the plaintext on success, std::nullopt if authentication fails
/// (tampered ciphertext, wrong key, corrupted data).
[[nodiscard]] std::optional<std::string> decrypt_token(
    const AuthKey& key,
    const EncryptedToken& token);

// ── Wire format helpers ─────────────────────────────────────────────────────

/// Serialize EncryptedToken to a base64 string suitable for HTTP headers.
/// Format: base64([IV || ciphertext || tag])
[[nodiscard]] std::string token_to_wire(const EncryptedToken& token);

/// Deserialize a base64 wire-format string back into EncryptedToken.
/// Returns std::nullopt if the format is invalid.
[[nodiscard]] std::optional<EncryptedToken> token_from_wire(
    const std::string& wire);

// ── Transport-layer auth interface ──────────────────────────────────────────

/// Authentication context attached to a transport.
/// The transport calls validate() on each incoming request.
class AuthProvider {
public:
    explicit AuthProvider(KeyConfig config);
    ~AuthProvider();

    AuthProvider(const AuthProvider&) = delete;
    AuthProvider& operator=(const AuthProvider&) = delete;

    /// Encrypt a token for sending (client-side).
    [[nodiscard]] std::string encrypt_for_header(const std::string& token);

    /// Validate and decrypt an auth header value (server-side).
    /// Returns the decrypted token on success, std::nullopt on failure.
    [[nodiscard]] std::optional<std::string> validate_header(
        const std::string& header_value);

    /// Check if auth is enabled (key is valid).
    [[nodiscard]] bool is_enabled() const noexcept;

private:
    AuthKey key_;
    bool    enabled_{false};
};

} // namespace mcp::auth
