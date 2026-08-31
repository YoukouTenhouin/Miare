#pragma once

#include <miare/error.hpp>
#include <miare/types.hpp>

#ifndef MIARE_HAS_SODIUM
#define MIARE_HAS_SODIUM 1
#endif
#ifndef MIARE_HAS_ZSTD
#define MIARE_HAS_ZSTD 1
#endif

#if MIARE_HAS_SODIUM
#include <sodium.h>
#endif
#if MIARE_HAS_ZSTD
#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#define MIARE_UNDEFINE_ZSTD_STATIC_LINKING_ONLY
#endif
#include <zstd.h>
#include <zstd_errors.h>
#ifdef MIARE_UNDEFINE_ZSTD_STATIC_LINKING_ONLY
#undef ZSTD_STATIC_LINKING_ONLY
#undef MIARE_UNDEFINE_ZSTD_STATIC_LINKING_ONLY
#endif
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>

namespace miare::detail {

class ProviderAccess;

inline constexpr std::size_t cryptoKeyBytes = 32;
inline constexpr std::size_t databaseIdentityBytes = 16;
inline constexpr std::size_t kdfSaltBytes = 16;
inline constexpr std::size_t aeadNonceBytes = 24;
inline constexpr std::size_t authenticationTagBytes = 16;
inline constexpr std::size_t maxProviderUnitBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t maxRandomRequestBytes = 1024U * 1024U;

[[nodiscard]] constexpr std::size_t zstdCompressBound(
    std::size_t inputBytes) noexcept {
    const auto margin = inputBytes < 128U * 1024U
        ? ((128U * 1024U - inputBytes) >> 11U)
        : 0U;
    return inputBytes + (inputBytes >> 8U) + margin;
}

class EntropySource {
public:
    virtual ~EntropySource() = default;
    virtual void randomBytes(MutableByteView output) = 0;
};

class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;

    virtual void randomBytes(MutableByteView output) = 0;

    virtual void deriveDatabaseRoot(
        ByteView callerKey,
        ByteView databaseIdentity,
        ByteView salt,
        std::uint32_t encryptionSuite,
        std::uint32_t derivationVersion,
        MutableByteView output) = 0;

    virtual void deriveSubkey(
        ByteView databaseRoot,
        std::uint64_t subkeyId,
        MutableByteView output) = 0;

    virtual void hashBlake2b256(ByteView input, MutableByteView output) = 0;

    virtual void encryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView plaintext,
        ByteView associatedData,
        MutableByteView ciphertext,
        MutableByteView tag) = 0;

    [[nodiscard]] virtual bool decryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView ciphertext,
        ByteView tag,
        ByteView associatedData,
        MutableByteView plaintext) = 0;
};

class SystemEntropySource final : public EntropySource {
public:
    void randomBytes(MutableByteView output) override {
        if (output.size() > maxRandomRequestBytes) {
            throw ContractError{Errc::InvalidArgument, "randomness request exceeds its bound"};
        }
        std::random_device random;
        for (auto& byte : output) {
            byte = std::byte{static_cast<unsigned char>(random())};
        }
    }
};

class CryptoEntropySource final : public EntropySource {
public:
    explicit CryptoEntropySource(CryptoProvider& crypto) noexcept : crypto_(&crypto) {}

    void randomBytes(MutableByteView output) override {
        crypto_->randomBytes(output);
    }

private:
    CryptoProvider* crypto_;
};

class CompressionProvider {
public:
    virtual ~CompressionProvider() = default;
    [[nodiscard]] virtual std::size_t compressBound(std::size_t inputBytes) const = 0;
    [[nodiscard]] virtual std::size_t compress(
        ByteView input,
        MutableByteView output) = 0;
    virtual void decompress(ByteView frame, MutableByteView output) = 0;
};

inline void requireSize(ByteView bytes, std::size_t expected, const char* name) {
    if (bytes.size() != expected) {
        throw ContractError{
            Errc::InvalidArgument,
            std::string{name} + " has an invalid size"};
    }
}

inline void requireSize(MutableByteView bytes, std::size_t expected, const char* name) {
    requireSize(ByteView{bytes.data(), bytes.size()}, expected, name);
}

#if MIARE_HAS_SODIUM
class SodiumCryptoProvider final : public CryptoProvider {
public:
    SodiumCryptoProvider() {
        if (sodium_init() < 0) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "libsodium initialization failed"};
        }
        static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES == cryptoKeyBytes);
        static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == aeadNonceBytes);
        static_assert(crypto_aead_xchacha20poly1305_ietf_ABYTES == authenticationTagBytes);
        static_assert(crypto_kdf_KEYBYTES == cryptoKeyBytes);
        static_assert(crypto_kdf_CONTEXTBYTES == 8);
    }

    void randomBytes(MutableByteView output) override {
        if (output.size() > maxRandomRequestBytes) {
            throw ContractError{Errc::InvalidArgument, "randomness request exceeds its bound"};
        }
        randombytes_buf(output.data(), output.size());
    }

    void deriveDatabaseRoot(
        ByteView callerKey,
        ByteView databaseIdentity,
        ByteView salt,
        std::uint32_t encryptionSuite,
        std::uint32_t derivationVersion,
        MutableByteView output) override {
        requireSize(callerKey, cryptoKeyBytes, "caller encryption key");
        requireSize(databaseIdentity, databaseIdentityBytes, "database identity");
        requireSize(salt, kdfSaltBytes, "KDF salt");
        requireSize(output, cryptoKeyBytes, "database root output");

        std::array<unsigned char, 24> message{};
        std::copy(databaseIdentity.begin(), databaseIdentity.end(),
                  reinterpret_cast<std::byte*>(message.data()));
        writeLittleEndian(encryptionSuite, message.data() + 16);
        writeLittleEndian(derivationVersion, message.data() + 20);
        constexpr std::array<unsigned char, 16> personalization{
            'M', 'i', 'a', 'r', 'e', 'D', 'b', 'R', 'o', 'o', 't', 'V', '1', 0, 0, 0};

        if (crypto_generichash_blake2b_salt_personal(
                asUnsigned(output.data()),
                output.size(),
                message.data(),
                message.size(),
                asUnsigned(callerKey.data()),
                callerKey.size(),
                asUnsigned(salt.data()),
                personalization.data()) != 0) {
            throwProviderFailure("BLAKE2b database-root derivation failed");
        }
    }

    void deriveSubkey(
        ByteView databaseRoot,
        std::uint64_t subkeyId,
        MutableByteView output) override {
        requireSize(databaseRoot, cryptoKeyBytes, "database root");
        requireSize(output, cryptoKeyBytes, "derived subkey output");
        if (subkeyId < 1 || subkeyId > 4) {
            throw ContractError{Errc::InvalidArgument, "KDF subkey domain is invalid"};
        }
        constexpr char context[crypto_kdf_CONTEXTBYTES] = {
            'M', 'i', 'a', 'r', 'e', 'V', '1', 'K'};
        if (crypto_kdf_derive_from_key(
                asUnsigned(output.data()),
                output.size(),
                subkeyId,
                context,
                asUnsigned(databaseRoot.data())) != 0) {
            throwProviderFailure("BLAKE2b subkey derivation failed");
        }
    }

    void hashBlake2b256(ByteView input, MutableByteView output) override {
        requireSize(output, cryptoKeyBytes, "BLAKE2b-256 output");
        if (input.size() > maxProviderUnitBytes ||
            crypto_generichash(
                asUnsigned(output.data()),
                output.size(),
                asUnsigned(input.data()),
                input.size(),
                nullptr,
                0) != 0) {
            throwProviderFailure("BLAKE2b-256 hashing failed");
        }
    }

    void encryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView plaintext,
        ByteView associatedData,
        MutableByteView ciphertext,
        MutableByteView tag) override {
        validateAeadBuffers(key, nonce, plaintext, ciphertext, tag);
        requireAssociatedDataBound(associatedData);
        unsigned long long tagLength = 0;
        if (crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
                asUnsigned(ciphertext.data()),
                asUnsigned(tag.data()),
                &tagLength,
                asUnsigned(plaintext.data()),
                plaintext.size(),
                asUnsigned(associatedData.data()),
                associatedData.size(),
                nullptr,
                asUnsigned(nonce.data()),
                asUnsigned(key.data())) != 0 ||
            tagLength != authenticationTagBytes) {
            throwProviderFailure("XChaCha20-Poly1305 encryption failed");
        }
    }

    [[nodiscard]] bool decryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView ciphertext,
        ByteView tag,
        ByteView associatedData,
        MutableByteView plaintext) override {
        validateAeadBuffers(key, nonce, ciphertext, plaintext, tag);
        requireAssociatedDataBound(associatedData);
        std::fill(plaintext.begin(), plaintext.end(), std::byte{0});
        const int status = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
            asUnsigned(plaintext.data()),
            nullptr,
            asUnsigned(ciphertext.data()),
            ciphertext.size(),
            asUnsigned(tag.data()),
            asUnsigned(associatedData.data()),
            associatedData.size(),
            asUnsigned(nonce.data()),
            asUnsigned(key.data()));
        if (status != 0) {
            std::fill(plaintext.begin(), plaintext.end(), std::byte{0});
            return false;
        }
        return true;
    }

private:
    static unsigned char* asUnsigned(std::byte* bytes) noexcept {
        return reinterpret_cast<unsigned char*>(bytes);
    }

    static const unsigned char* asUnsigned(const std::byte* bytes) noexcept {
        return reinterpret_cast<const unsigned char*>(bytes);
    }

    static void writeLittleEndian(std::uint32_t value, unsigned char* output) noexcept {
        for (unsigned index = 0; index != 4; ++index) {
            output[index] = static_cast<unsigned char>(value >> (index * 8U));
        }
    }

    static void validateAeadBuffers(
        ByteView key,
        ByteView nonce,
        ByteView input,
        MutableByteView output,
        ByteView tag) {
        requireSize(key, cryptoKeyBytes, "AEAD key");
        requireSize(nonce, aeadNonceBytes, "AEAD nonce");
        requireSize(tag, authenticationTagBytes, "authentication tag");
        requireSize(output, input.size(), "AEAD output");
        if (input.size() > maxProviderUnitBytes ||
            input.size() > crypto_aead_xchacha20poly1305_ietf_MESSAGEBYTES_MAX) {
            throw ContractError{Errc::InvalidArgument, "authenticated unit exceeds its bound"};
        }
    }

    static void requireAssociatedDataBound(ByteView associatedData) {
        if (associatedData.size() > maxProviderUnitBytes) {
            throw ContractError{Errc::InvalidArgument, "associated data exceeds its bound"};
        }
    }

    [[noreturn]] static void throwProviderFailure(const char* message) {
        throw DatabaseError{Errc::ProviderUnavailable, message};
    }
};
#endif

#if MIARE_HAS_ZSTD
class ZstdCompressionProvider final : public CompressionProvider {
public:
    [[nodiscard]] std::size_t compressBound(std::size_t inputBytes) const override {
        requireDecodedBound(inputBytes);
        const auto bound = ZSTD_compressBound(inputBytes);
        if (bound != zstdCompressBound(inputBytes)) {
            throwProviderFailure("Zstandard compression bound is incompatible");
        }
        return bound;
    }

    [[nodiscard]] std::size_t compress(
        ByteView input,
        MutableByteView output) override {
        requireDecodedBound(input.size());
        if (output.size() < compressBound(input.size())) {
            throw ContractError{Errc::InvalidArgument, "compression output is too small"};
        }

        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{
            ZSTD_createCCtx(), &ZSTD_freeCCtx};
        if (!context) {
            throwProviderFailure("Zstandard compression context allocation failed");
        }
        setParameter(context.get(), ZSTD_c_compressionLevel, 3);
        setParameter(context.get(), ZSTD_c_windowLog, 24);
        setParameter(context.get(), ZSTD_c_contentSizeFlag, 1);
        setParameter(context.get(), ZSTD_c_checksumFlag, 0);
        setParameter(context.get(), ZSTD_c_dictIDFlag, 0);
        setParameter(context.get(), ZSTD_c_nbWorkers, 0);
        checkProvider(ZSTD_CCtx_setPledgedSrcSize(context.get(), input.size()),
                      "Zstandard input-size setup failed");

        const auto written = ZSTD_compress2(
            context.get(), output.data(), output.size(), input.data(), input.size());
        checkProvider(written, "Zstandard compression failed");
        return written;
    }

    void decompress(ByteView frame, MutableByteView output) override {
        requireDecodedBound(output.size());
        if (frame.empty() || frame.size() > ZSTD_compressBound(maxProviderUnitBytes)) {
            throwCorrupt("compressed frame exceeds its bound");
        }

        ZSTD_frameHeader header{};
        const auto headerStatus = ZSTD_getFrameHeader(&header, frame.data(), frame.size());
        if (ZSTD_isError(headerStatus) || headerStatus != 0 ||
            header.frameType != ZSTD_frame ||
            header.frameContentSize != output.size() ||
            header.windowSize > maxProviderUnitBytes ||
            header.dictID != 0 ||
            header.checksumFlag != 0) {
            throwCorrupt("compressed frame violates Zstandard profile 1");
        }

        const auto frameSize = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
        if (ZSTD_isError(frameSize) || frameSize != frame.size()) {
            throwCorrupt("compressed input is not exactly one frame");
        }

        std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{
            ZSTD_createDCtx(), &ZSTD_freeDCtx};
        if (!context) {
            throwProviderFailure("Zstandard decompression context allocation failed");
        }
        checkProvider(
            ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 24),
            "Zstandard decode-window setup failed");
        const auto decoded = ZSTD_decompressDCtx(
            context.get(), output.data(), output.size(), frame.data(), frame.size());
        if (ZSTD_isError(decoded)) {
            std::fill(output.begin(), output.end(), std::byte{0});
            if (ZSTD_getErrorCode(decoded) == ZSTD_error_memory_allocation) {
                throwProviderFailure("Zstandard decompression allocation failed");
            }
            throwCorrupt("Zstandard frame decode failed");
        }
        if (decoded != output.size()) {
            std::fill(output.begin(), output.end(), std::byte{0});
            throwCorrupt("Zstandard frame decoded to an invalid size");
        }
    }

private:
    static void requireDecodedBound(std::size_t bytes) {
        if (bytes > maxProviderUnitBytes) {
            throw ContractError{Errc::InvalidArgument, "codec unit exceeds its output bound"};
        }
    }

    static void setParameter(ZSTD_CCtx* context, ZSTD_cParameter parameter, int value) {
        checkProvider(
            ZSTD_CCtx_setParameter(context, parameter, value),
            "Zstandard profile setup failed");
    }

    static void checkProvider(std::size_t status, const char* message) {
        if (ZSTD_isError(status)) {
            throwProviderFailure(message);
        }
    }

    [[noreturn]] static void throwProviderFailure(const char* message) {
        throw DatabaseError{Errc::ProviderUnavailable, message};
    }

    [[noreturn]] static void throwCorrupt(const char* message) {
        throw DatabaseError{Errc::Corrupt, message};
    }
};
#endif

} // namespace miare::detail

namespace miare {

/// Owns the cryptographic and compression capabilities used by one session.
///
/// Provider identity is never stored in the database file; only frozen
/// algorithm and profile identifiers are persistent. A provider set is
/// move-only and becomes inert after being moved into a database operation.
class ProviderSet {
public:
    /// Creates a provider set with no optional crypto or compression capability.
    [[nodiscard]] static ProviderSet none() {
        return ProviderSet{nullptr, nullptr};
    }

#if MIARE_HAS_SODIUM
    /// Creates a production provider set with only the libsodium capability.
    [[nodiscard]] static ProviderSet systemCrypto() {
        return ProviderSet{
            std::make_unique<detail::SodiumCryptoProvider>(), nullptr};
    }
#endif

#if MIARE_HAS_ZSTD
    /// Creates a production provider set with only the Zstandard capability.
    [[nodiscard]] static ProviderSet systemCompression() {
        return ProviderSet{
            nullptr, std::make_unique<detail::ZstdCompressionProvider>()};
    }
#endif

#if MIARE_HAS_SODIUM && MIARE_HAS_ZSTD
    /// Creates the production provider set backed by libsodium and Zstandard.
    [[nodiscard]] static ProviderSet system() {
        return ProviderSet{
            std::make_unique<detail::SodiumCryptoProvider>(),
            std::make_unique<detail::ZstdCompressionProvider>()};
    }
#endif

    /// Moves ownership of every provider capability.
    ProviderSet(ProviderSet&&) noexcept = default;
    /// Replaces this set by moving provider ownership from another set.
    ProviderSet& operator=(ProviderSet&&) noexcept = default;
    ProviderSet(const ProviderSet&) = delete;
    ProviderSet& operator=(const ProviderSet&) = delete;

private:
    friend class detail::ProviderAccess;

    ProviderSet(
        std::unique_ptr<detail::CryptoProvider> crypto,
        std::unique_ptr<detail::CompressionProvider> compression,
        std::unique_ptr<detail::EntropySource> entropy = nullptr)
        : crypto_(std::move(crypto)), compression_(std::move(compression)) {
        if (entropy) {
            entropy_ = std::move(entropy);
        } else {
            entropy_ = std::make_unique<detail::SystemEntropySource>();
        }
    }

    std::unique_ptr<detail::CryptoProvider> crypto_;
    std::unique_ptr<detail::CompressionProvider> compression_;
    std::unique_ptr<detail::EntropySource> entropy_;
};

namespace detail {

class ProviderAccess {
public:
    [[nodiscard]] static ProviderSet make(
        std::unique_ptr<CryptoProvider> crypto,
        std::unique_ptr<CompressionProvider> compression,
        std::unique_ptr<EntropySource> entropy = nullptr) {
        return ProviderSet{
            std::move(crypto), std::move(compression), std::move(entropy)};
    }

    static CryptoProvider& crypto(ProviderSet& providers) {
        if (!providers.crypto_) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "cryptographic provider is missing"};
        }
        return *providers.crypto_;
    }

    static EntropySource& entropy(ProviderSet& providers) {
        if (!providers.entropy_) {
            throw ContractError{Errc::InvalidState, "provider set is inert"};
        }
        return *providers.entropy_;
    }

    static CompressionProvider& compression(ProviderSet& providers) {
        if (!providers.compression_) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "compression provider is missing"};
        }
        return *providers.compression_;
    }
};

} // namespace detail
} // namespace miare
