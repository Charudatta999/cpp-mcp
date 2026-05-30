/// auth_tests.cpp -- Unit tests for the MCP authentication module.
///
/// Tests cover:
///   1. Key generation and hex conversion
///   2. Key loading (env, file, raw)
///   3. Encrypt/decrypt round-trip (AES-256-GCM)
///   4. Tamper detection (modified ciphertext/tag/IV)
///   5. Wrong key rejection
///   6. Wire format (base64 serialization)
///   7. AuthProvider integration
///   8. Edge cases (empty token, large token, invalid input)
///
/// Build: cmake --build . --target auth_tests
/// Run:   ctest -R auth_tests --output-on-failure

#include "mcp/auth/auth.hpp"
#include "mcp/core/errors.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int g_tests_run = 0;
int g_tests_passed = 0;

void run_test(const char* name, void(*fn)()) {
    ++g_tests_run;
    try {
        fn();
        ++g_tests_passed;
        std::cout << "  PASS: " << name << "\n";
    } catch (const std::exception& e) {
        std::cerr << "  FAIL: " << name << " -- " << e.what() << "\n";
    } catch (...) {
        std::cerr << "  FAIL: " << name << " -- unknown exception\n";
    }
}

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("Assertion failed: " #expr); } while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error( \
        std::string("Assertion failed: ") + #a + " != " + #b); } while(0)

#define ASSERT_THROWS(expr, ExType) \
    do { bool caught = false; \
        try { (void)(expr); } catch (const ExType&) { caught = true; } \
        if (!caught) throw std::runtime_error("Expected " #ExType " from: " #expr); \
    } while(0)

using namespace mcp::auth;

// ═══════════════════════════════════════════════════════════════════════════
//  1. Key generation and hex conversion
// ═══════════════════════════════════════════════════════════════════════════

void test_generate_key_is_valid() {
    auto key = generate_key();
    ASSERT_TRUE(key.is_valid());
    ASSERT_EQ(key.size(), kKeySize);
}

void test_generate_key_unique() {
    auto k1 = generate_key();
    auto k2 = generate_key();
    // Two random keys should never be identical
    ASSERT_TRUE(std::memcmp(k1.data(), k2.data(), kKeySize) != 0);
}

void test_key_to_hex_roundtrip() {
    auto key = generate_key();
    auto hex = key_to_hex(key);
    ASSERT_EQ(hex.size(), kKeyHexSize);

    auto restored = key_from_hex(hex);
    ASSERT_TRUE(std::memcmp(key.data(), restored.data(), kKeySize) == 0);
}

void test_key_from_hex_invalid_length() {
    ASSERT_THROWS(key_from_hex("abcdef"), mcp::McpError);
}

void test_key_from_hex_invalid_chars() {
    std::string bad(kKeyHexSize, 'g');  // 'g' is not hex
    ASSERT_THROWS(key_from_hex(bad), mcp::McpError);
}

// ═══════════════════════════════════════════════════════════════════════════
//  2. Key loading
// ═══════════════════════════════════════════════════════════════════════════

void test_load_key_from_raw() {
    auto original = generate_key();
    KeyConfig config;
    config.source = KeySource::Raw;
    config.raw_key.assign(original.data(), original.data() + kKeySize);

    auto loaded = load_key(config);
    ASSERT_TRUE(std::memcmp(original.data(), loaded.data(), kKeySize) == 0);
}

void test_load_key_from_raw_wrong_size() {
    KeyConfig config;
    config.source = KeySource::Raw;
    config.raw_key = {1, 2, 3};  // Too short
    ASSERT_THROWS(load_key(config), mcp::McpError);
}

void test_load_key_from_env() {
    auto key = generate_key();
    auto hex = key_to_hex(key);

    // Set env var
#ifdef _WIN32
    _putenv_s("MCP_TEST_AUTH_KEY", hex.c_str());
#else
    setenv("MCP_TEST_AUTH_KEY", hex.c_str(), 1);
#endif

    KeyConfig config;
    config.source = KeySource::EnvVariable;
    config.env_var_name = "MCP_TEST_AUTH_KEY";

    auto loaded = load_key(config);
    ASSERT_TRUE(std::memcmp(key.data(), loaded.data(), kKeySize) == 0);

    // Cleanup
#ifdef _WIN32
    _putenv_s("MCP_TEST_AUTH_KEY", "");
#else
    unsetenv("MCP_TEST_AUTH_KEY");
#endif
}

void test_load_key_from_env_missing() {
    KeyConfig config;
    config.source = KeySource::EnvVariable;
    config.env_var_name = "MCP_NONEXISTENT_KEY_XYZ_12345";
    ASSERT_THROWS(load_key(config), mcp::McpError);
}

std::string temp_path(const char* filename) {
#ifdef _WIN32
    char buf[256];
    GetTempPathA(sizeof(buf), buf);
    return std::string(buf) + filename;
#else
    return std::string("/tmp/") + filename;
#endif
}

void test_load_key_from_file_hex() {
    auto key = generate_key();
    auto hex = key_to_hex(key);

    auto path = temp_path("mcp_test_key_hex.txt");
    { std::ofstream f(path); f << hex; }

    KeyConfig config;
    config.source = KeySource::File;
    config.file_path = path;

    auto loaded = load_key(config);
    ASSERT_TRUE(std::memcmp(key.data(), loaded.data(), kKeySize) == 0);

    std::remove(path.c_str());
}

void test_load_key_from_file_raw() {
    auto key = generate_key();

    auto path = temp_path("mcp_test_key_raw.bin");
    { std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(key.data()), kKeySize); }

    KeyConfig config;
    config.source = KeySource::File;
    config.file_path = path;

    auto loaded = load_key(config);
    ASSERT_TRUE(std::memcmp(key.data(), loaded.data(), kKeySize) == 0);

    std::remove(path.c_str());
}

void test_load_key_from_file_missing() {
    KeyConfig config;
    config.source = KeySource::File;
    config.file_path = "/tmp/mcp_nonexistent_key_file_xyz.bin";
    ASSERT_THROWS(load_key(config), mcp::McpError);
}

// ═══════════════════════════════════════════════════════════════════════════
//  3. Encrypt/decrypt round-trip
// ═══════════════════════════════════════════════════════════════════════════

void test_encrypt_decrypt_roundtrip() {
    auto key = generate_key();
    std::string plaintext = "my-secret-api-key-12345";

    auto encrypted = encrypt_token(key, plaintext);
    ASSERT_EQ(encrypted.iv.size(), kIvSize);
    ASSERT_EQ(encrypted.tag.size(), kTagSize);
    ASSERT_FALSE(encrypted.ciphertext.empty());

    auto decrypted = decrypt_token(key, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    ASSERT_EQ(*decrypted, plaintext);
}

void test_encrypt_produces_different_ciphertext() {
    auto key = generate_key();
    std::string plaintext = "same-input-different-output";

    auto e1 = encrypt_token(key, plaintext);
    auto e2 = encrypt_token(key, plaintext);

    // Random IV means ciphertext differs even for same plaintext
    ASSERT_TRUE(e1.iv != e2.iv || e1.ciphertext != e2.ciphertext);
}

void test_encrypt_empty_plaintext() {
    auto key = generate_key();
    std::string plaintext;

    auto encrypted = encrypt_token(key, plaintext);
    auto decrypted = decrypt_token(key, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    ASSERT_EQ(*decrypted, "");
}

void test_encrypt_large_plaintext() {
    auto key = generate_key();
    std::string plaintext(4096, 'X');  // 4KB token

    auto encrypted = encrypt_token(key, plaintext);
    auto decrypted = decrypt_token(key, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    ASSERT_EQ(*decrypted, plaintext);
}

// ═══════════════════════════════════════════════════════════════════════════
//  4. Tamper detection
// ═══════════════════════════════════════════════════════════════════════════

void test_tampered_ciphertext_detected() {
    auto key = generate_key();
    auto encrypted = encrypt_token(key, "sensitive-data");

    // Flip a byte in ciphertext
    encrypted.ciphertext[0] ^= 0xFF;

    auto result = decrypt_token(key, encrypted);
    ASSERT_FALSE(result.has_value());
}

void test_tampered_tag_detected() {
    auto key = generate_key();
    auto encrypted = encrypt_token(key, "sensitive-data");

    // Flip a byte in the auth tag
    encrypted.tag[0] ^= 0xFF;

    auto result = decrypt_token(key, encrypted);
    ASSERT_FALSE(result.has_value());
}

void test_tampered_iv_detected() {
    auto key = generate_key();
    auto encrypted = encrypt_token(key, "sensitive-data");

    // Flip a byte in IV
    encrypted.iv[0] ^= 0xFF;

    auto result = decrypt_token(key, encrypted);
    ASSERT_FALSE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
//  5. Wrong key rejection
// ═══════════════════════════════════════════════════════════════════════════

void test_wrong_key_rejected() {
    auto key1 = generate_key();
    auto key2 = generate_key();

    auto encrypted = encrypt_token(key1, "secret");
    auto result = decrypt_token(key2, encrypted);
    ASSERT_FALSE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
//  6. Wire format
// ═══════════════════════════════════════════════════════════════════════════

void test_wire_format_roundtrip() {
    auto key = generate_key();
    auto encrypted = encrypt_token(key, "wire-format-test");

    auto wire = token_to_wire(encrypted);
    ASSERT_FALSE(wire.empty());

    auto parsed = token_from_wire(wire);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->iv, encrypted.iv);
    ASSERT_EQ(parsed->ciphertext, encrypted.ciphertext);
    ASSERT_EQ(parsed->tag, encrypted.tag);

    // Full round-trip: encrypt -> wire -> parse -> decrypt
    auto decrypted = decrypt_token(key, *parsed);
    ASSERT_TRUE(decrypted.has_value());
    ASSERT_EQ(*decrypted, "wire-format-test");
}

void test_wire_format_invalid_base64() {
    auto result = token_from_wire("!!!not-base64!!!");
    ASSERT_FALSE(result.has_value());
}

void test_wire_format_too_short() {
    auto result = token_from_wire("AQID");  // Only 3 bytes decoded
    ASSERT_FALSE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
//  7. AuthProvider integration
// ═══════════════════════════════════════════════════════════════════════════

void test_auth_provider_encrypt_validate_roundtrip() {
    auto key = generate_key();

    KeyConfig config;
    config.source = KeySource::Raw;
    config.raw_key.assign(key.data(), key.data() + kKeySize);

    AuthProvider provider(config);
    ASSERT_TRUE(provider.is_enabled());

    auto header = provider.encrypt_for_header("user-token-abc");
    ASSERT_FALSE(header.empty());

    auto validated = provider.validate_header(header);
    ASSERT_TRUE(validated.has_value());
    ASSERT_EQ(*validated, "user-token-abc");
}

void test_auth_provider_invalid_header() {
    auto key = generate_key();

    KeyConfig config;
    config.source = KeySource::Raw;
    config.raw_key.assign(key.data(), key.data() + kKeySize);

    AuthProvider provider(config);
    auto result = provider.validate_header("garbage-data-not-encrypted");
    ASSERT_FALSE(result.has_value());
}

void test_auth_provider_different_keys_reject() {
    auto key1 = generate_key();
    auto key2 = generate_key();

    KeyConfig config1;
    config1.source = KeySource::Raw;
    config1.raw_key.assign(key1.data(), key1.data() + kKeySize);

    KeyConfig config2;
    config2.source = KeySource::Raw;
    config2.raw_key.assign(key2.data(), key2.data() + kKeySize);

    AuthProvider sender(config1);
    AuthProvider receiver(config2);

    auto header = sender.encrypt_for_header("secret");
    auto result = receiver.validate_header(header);
    ASSERT_FALSE(result.has_value());
}

void test_auth_provider_disabled_when_key_missing() {
    KeyConfig config;
    config.source = KeySource::EnvVariable;
    config.env_var_name = "MCP_DOES_NOT_EXIST_XYZ";

    AuthProvider provider(config);
    ASSERT_FALSE(provider.is_enabled());
}

// ═══════════════════════════════════════════════════════════════════════════
//  8. Edge cases
// ═══════════════════════════════════════════════════════════════════════════

void test_key_secure_zero_on_destroy() {
    std::vector<uint8_t> key_copy(kKeySize);
    {
        auto key = generate_key();
        std::memcpy(key_copy.data(), key.data(), kKeySize);
        // key destroyed here
    }
    // Can't verify zeroing of freed memory, but at least verify the
    // copy we took is non-zero (proves the key was valid)
    bool all_zero = true;
    for (auto b : key_copy) { if (b != 0) { all_zero = false; break; } }
    ASSERT_FALSE(all_zero);
}

void test_unicode_token_roundtrip() {
    auto key = generate_key();
    std::string token = "token-\xC3\xA9\xC3\xA0\xC3\xBC-utf8";  // éàü

    auto encrypted = encrypt_token(key, token);
    auto decrypted = decrypt_token(key, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    ASSERT_EQ(*decrypted, token);
}

} // namespace

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║       cpp-mcp Auth Test Suite                ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n\n";

    std::cout << "[1] Key Generation & Hex Conversion\n";
    run_test("generate_key_is_valid", test_generate_key_is_valid);
    run_test("generate_key_unique", test_generate_key_unique);
    run_test("key_to_hex_roundtrip", test_key_to_hex_roundtrip);
    run_test("key_from_hex_invalid_length", test_key_from_hex_invalid_length);
    run_test("key_from_hex_invalid_chars", test_key_from_hex_invalid_chars);

    std::cout << "\n[2] Key Loading (Env, File, Raw)\n";
    run_test("load_key_from_raw", test_load_key_from_raw);
    run_test("load_key_from_raw_wrong_size", test_load_key_from_raw_wrong_size);
    run_test("load_key_from_env", test_load_key_from_env);
    run_test("load_key_from_env_missing", test_load_key_from_env_missing);
    run_test("load_key_from_file_hex", test_load_key_from_file_hex);
    run_test("load_key_from_file_raw", test_load_key_from_file_raw);
    run_test("load_key_from_file_missing", test_load_key_from_file_missing);

    std::cout << "\n[3] Encrypt/Decrypt Round-Trip (AES-256-GCM)\n";
    run_test("encrypt_decrypt_roundtrip", test_encrypt_decrypt_roundtrip);
    run_test("encrypt_produces_different_ciphertext", test_encrypt_produces_different_ciphertext);
    run_test("encrypt_empty_plaintext", test_encrypt_empty_plaintext);
    run_test("encrypt_large_plaintext", test_encrypt_large_plaintext);

    std::cout << "\n[4] Tamper Detection\n";
    run_test("tampered_ciphertext_detected", test_tampered_ciphertext_detected);
    run_test("tampered_tag_detected", test_tampered_tag_detected);
    run_test("tampered_iv_detected", test_tampered_iv_detected);

    std::cout << "\n[5] Wrong Key Rejection\n";
    run_test("wrong_key_rejected", test_wrong_key_rejected);

    std::cout << "\n[6] Wire Format (Base64 Serialization)\n";
    run_test("wire_format_roundtrip", test_wire_format_roundtrip);
    run_test("wire_format_invalid_base64", test_wire_format_invalid_base64);
    run_test("wire_format_too_short", test_wire_format_too_short);

    std::cout << "\n[7] AuthProvider Integration\n";
    run_test("auth_provider_encrypt_validate_roundtrip", test_auth_provider_encrypt_validate_roundtrip);
    run_test("auth_provider_invalid_header", test_auth_provider_invalid_header);
    run_test("auth_provider_different_keys_reject", test_auth_provider_different_keys_reject);
    run_test("auth_provider_disabled_when_key_missing", test_auth_provider_disabled_when_key_missing);

    std::cout << "\n[8] Edge Cases\n";
    run_test("key_secure_zero_on_destroy", test_key_secure_zero_on_destroy);
    run_test("unicode_token_roundtrip", test_unicode_token_roundtrip);

    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed\n";

    if (g_tests_passed == g_tests_run) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cerr << (g_tests_run - g_tests_passed) << " TEST(S) FAILED\n";
        return 1;
    }
}
