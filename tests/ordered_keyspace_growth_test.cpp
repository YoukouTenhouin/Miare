#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <iterator>
#include <optional>
#include <map>
#include <set>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void requireCorruptImage(std::vector<std::byte> image);

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] std::vector<std::byte> keyFor(std::uint16_t index) {
    std::vector<std::byte> key(2'048, std::byte{0x5a});
    key[0] = std::byte{static_cast<unsigned char>(index >> 8U)};
    key[1] = std::byte{static_cast<unsigned char>(index)};
    return key;
}

[[nodiscard]] std::vector<std::byte> valueFor(std::uint16_t index) {
    return std::vector<std::byte>(
        900,
        std::byte{static_cast<unsigned char>((index * 37U) & 0xffU)});
}

void committedKeysSurviveMultipleTreeLevelsAndReopen() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1));

    auto write = database.beginWrite();
    for (std::uint16_t index = 0; index != 120; ++index) {
        const auto key = keyFor(index);
        const auto value = valueFor(index);
        write.put(key, value);
    }
    write.commit();

    auto read = database.beginRead();
    for (std::uint16_t index = 0; index != 120; ++index) {
        const auto actual = read.get(keyFor(index));
        require(actual.has_value());
        require(std::equal(actual->begin(), actual->end(), valueFor(index).begin()));
    }
    read.end();
    const auto image = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2));
    require(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto persisted = reopened.beginRead();
    for (std::uint16_t index = 0; index != 120; ++index) {
        const auto actual = persisted.get(keyFor(index));
        require(actual.has_value());
        require(std::equal(actual->begin(), actual->end(), valueFor(index).begin()));
    }
    persisted.end();
    reopened.close();
}

void overflowValuesRemainAtomicAcrossReplacementAndDeletion() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(3));
    const auto key = keyFor(500);
    std::vector<std::byte> first(4'097);
    std::vector<std::byte> second(8'193);
    for (std::size_t index = 0; index != second.size(); ++index) {
        second[index] = std::byte{static_cast<unsigned char>((index * 131U) & 0xffU)};
        if (index < first.size()) {
            first[index] = std::byte{static_cast<unsigned char>((index * 67U) & 0xffU)};
        }
    }

    auto initial = database.beginWrite();
    initial.put(key, first);
    initial.commit();
    auto replaced = database.beginWrite();
    replaced.put(key, second);
    replaced.commit();
    auto rolledBack = database.beginWrite();
    require(rolledBack.erase(key));
    rolledBack.rollback();
    auto read = database.beginRead();
    const auto actual = read.get(key);
    require(actual.has_value());
    require(std::equal(actual->begin(), actual->end(), second.begin(), second.end()));
    read.end();

    auto inlineReplacement = database.beginWrite();
    const std::array inlineValue{std::byte{0x41}, std::byte{0x42}};
    inlineReplacement.put(key, inlineValue);
    inlineReplacement.commit();
    auto remove = database.beginWrite();
    require(remove.erase(key));
    remove.commit();
    const auto image = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(4));
    require(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto absent = reopened.beginRead();
    require(!absent.get(key));
    absent.end();
    reopened.close();
}

void supersededStorageIsSafelyReused() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(5),
        options);
    const auto key = keyFor(700);
    std::vector<std::byte> value(256U * 1024U);
    std::uint32_t state = 0x9e3779b9U;
    std::size_t firstCommittedSize = 0;
    for (unsigned generation = 0; generation != 12; ++generation) {
        for (auto& byte : value) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            byte = std::byte{static_cast<unsigned char>(state)};
        }
        value.front() = std::byte{static_cast<unsigned char>(generation)};
        auto write = database.beginWrite();
        write.put(key, value);
        write.commit();
        if (generation == 0) {
            firstCommittedSize = fileView->bytes().size();
        }
    }
    require(fileView->bytes().size() <= firstCommittedSize * 4);
    const auto image = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(6));
    require(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto read = reopened.beginRead();
    const auto actual = read.get(key);
    require(actual.has_value());
    require(std::equal(actual->begin(), actual->end(), value.begin(), value.end()));
    read.end();
    reopened.close();
}

void postCreateGenerationsPublishAllocatorState() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(50));
    auto write = database.beginWrite();
    write.put(keyFor(1), valueFor(1));
    write.commit();
    const auto image = fileView->bytes();
    database.close();

    auto providers = deterministicProviders(51);
    miare::testing::MemoryDurableFile persisted;
    persisted.replaceStableBytes(image);
    auto opened = miare::detail::openFormat<miare::DefaultLimits>(
        persisted,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    require(opened.hasValue());
    require(!miare::detail::allZero(
        opened.value().publication,
        miare::detail::PublicationLayout::allocatorRoot,
        miare::detail::PublicationLayout::highWaterBlocks));
    const auto allocatorReference = miare::detail::decodeExtentReference(
        miare::ByteView{opened.value().publication}.subspan(
            miare::detail::PublicationLayout::allocatorRoot, 32));
    auto corrupted = image;
    corrupted[allocatorReference.blockIndex *
                  miare::DefaultLimits::allocationQuantumBytes +
              miare::detail::ExtentLayout::bytes] ^=
        std::byte{1};
    requireCorruptImage(std::move(corrupted));
}

[[nodiscard]] std::set<std::uint64_t> orderedPageBlocks(
    const std::vector<std::byte>& image,
    std::uint64_t providerSeed) {
    auto providers = deterministicProviders(providerSeed);
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    require(openedResult.hasValue());
    auto opened = std::move(openedResult).value();
    std::vector<miare::detail::ExtentReference> reachable;
    (void)miare::detail::loadExactValues<miare::DefaultLimits>(
        file, opened, providers, std::allocator<std::byte>{}, &reachable);
    std::set<std::uint64_t> pages;
    std::array<std::byte, miare::detail::ExtentLayout::bytes> preamble{};
    for (const auto& reference : reachable) {
        file.readExactAt(
            reference.blockIndex * miare::DefaultLimits::allocationQuantumBytes,
            preamble);
        const auto kind = miare::detail::readLittleEndian<std::uint16_t>(
            preamble, miare::detail::ExtentLayout::unitKind);
        if (kind == 1 || kind == 2) {
            pages.insert(reference.blockIndex);
        }
    }
    return pages;
}

[[nodiscard]] std::uint16_t orderedRootKind(
    const std::vector<std::byte>& image,
    std::uint64_t providerSeed) {
    auto providers = deterministicProviders(providerSeed);
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto opened = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    require(opened.hasValue());
    const auto root = miare::detail::decodeExtentReference(
        opened.value().format.orderedRoot);
    std::array<std::byte, miare::detail::ExtentLayout::bytes> preamble{};
    file.readExactAt(
        root.blockIndex * miare::DefaultLimits::allocationQuantumBytes,
        preamble);
    return miare::detail::readLittleEndian<std::uint16_t>(
        preamble, miare::detail::ExtentLayout::unitKind);
}

void mutationsRewriteOnlyAffectedTreePaths() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(60),
        options);
    auto seed = database.beginWrite();
    for (std::uint16_t index = 0; index != 120; ++index) {
        seed.put(keyFor(index), valueFor(index));
    }
    seed.commit();
    const auto initialPages = orderedPageBlocks(fileView->bytes(), 61);
    require(initialPages.size() > 8);

    auto overwrite = database.beginWrite();
    const std::array replacement{std::byte{0x11}, std::byte{0x22}};
    overwrite.put(keyFor(0), replacement);
    overwrite.commit();
    const auto overwrittenPages = orderedPageBlocks(fileView->bytes(), 62);
    std::vector<std::uint64_t> retainedAfterOverwrite;
    std::set_intersection(
        initialPages.begin(), initialPages.end(),
        overwrittenPages.begin(), overwrittenPages.end(),
        std::back_inserter(retainedAfterOverwrite));
    require(retainedAfterOverwrite.size() >= initialPages.size() - 4);

    auto erase = database.beginWrite();
    require(erase.erase(keyFor(1)));
    erase.commit();
    const auto deletedPages = orderedPageBlocks(fileView->bytes(), 63);
    std::vector<std::uint64_t> retainedAfterDelete;
    std::set_intersection(
        overwrittenPages.begin(), overwrittenPages.end(),
        deletedPages.begin(), deletedPages.end(),
        std::back_inserter(retainedAfterDelete));
    require(retainedAfterDelete.size() >= overwrittenPages.size() - 4);

    auto collapse = database.beginWrite();
    for (std::uint16_t index = 2; index != 120; ++index) {
        require(collapse.erase(keyFor(index)));
    }
    collapse.commit();
    require(orderedRootKind(fileView->bytes(), 64) == 2);
    database.close();
}

void requireCorruptImage(std::vector<std::byte> image) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(std::move(image));
    try {
        auto opened = miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(8));
        (void)opened;
        require(false);
    } catch (const miare::DatabaseError& error) {
        require(error.code() == miare::Errc::Corrupt);
    }
}

void authenticatedExtentBoundariesRejectPhysicalTampering() {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(7),
        options);
    std::vector<std::byte> first(4'097, std::byte{0x31});
    std::vector<std::byte> second(4'097, std::byte{0x72});
    auto write = database.beginWrite();
    write.put(keyFor(800), first);
    write.put(keyFor(801), second);
    write.commit();
    const auto image = fileView->bytes();
    database.close();

    auto truncated = image;
    truncated.resize(truncated.size() - 1);
    requireCorruptImage(std::move(truncated));

    auto malformedLength = image;
    const auto firstExtent = miare::detail::commonRegionBytes;
    malformedLength[firstExtent + miare::detail::ExtentLayout::storedLength] ^=
        std::byte{1};
    requireCorruptImage(std::move(malformedLength));

    auto substituted = image;
    const auto secondExtent = firstExtent + 2 * quantum;
    for (std::size_t offset = 0; offset != 2 * quantum; ++offset) {
        std::swap(substituted[firstExtent + offset], substituted[secondExtent + offset]);
    }
    requireCorruptImage(std::move(substituted));
}

[[nodiscard]] std::vector<std::byte> modeledKey(std::uint32_t identity) {
    return {
        std::byte{static_cast<unsigned char>(identity >> 24U)},
        std::byte{static_cast<unsigned char>(identity >> 16U)},
        std::byte{static_cast<unsigned char>(identity >> 8U)},
        std::byte{static_cast<unsigned char>(identity)}};
}

void randomizedHistoriesMatchAnIndependentReferenceModel() {
    using Model = std::map<std::vector<std::byte>, std::vector<std::byte>>;
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    std::optional<miare::Database<>> database;
    database.emplace(miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(9),
        options));
    Model model;
    std::uint32_t random = 0x12345678U;
    const auto nextRandom = [&]() {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        return random;
    };
    const auto verify = [&] {
        auto read = database->beginRead();
        for (std::uint32_t identity = 0; identity != 160; ++identity) {
            const auto key = modeledKey(identity);
            const auto expected = model.find(key);
            const auto actual = read.get(key);
            require(actual.has_value() == (expected != model.end()));
            if (actual) {
                require(std::equal(
                    actual->begin(), actual->end(),
                    expected->second.begin(), expected->second.end()));
            }
        }
        read.end();
    };

    for (unsigned transaction = 0; transaction != 80; ++transaction) {
        auto candidate = model;
        auto write = database->beginWrite();
        const auto operationCount = 1U + nextRandom() % 9U;
        for (unsigned operation = 0; operation != operationCount; ++operation) {
            const auto key = modeledKey(nextRandom() % 160U);
            if (nextRandom() % 4U == 0) {
                const bool expected = candidate.erase(key) != 0;
                require(write.erase(key) == expected);
                continue;
            }
            constexpr std::array<std::size_t, 5> lengths{0, 31, 1'024, 1'025, 3'001};
            std::vector<std::byte> value(lengths[nextRandom() % lengths.size()]);
            for (auto& byte : value) {
                byte = std::byte{static_cast<unsigned char>(nextRandom())};
            }
            write.put(key, value);
            candidate.insert_or_assign(key, std::move(value));
        }
        if (nextRandom() % 5U == 0) {
            write.rollback();
        } else {
            write.commit();
            model = std::move(candidate);
        }
        verify();

        if (transaction % 13U == 12U) {
            const auto image = fileView->bytes();
            database->close();
            database.reset();
            auto reopenedFile =
                std::make_unique<miare::testing::MemoryDurableFile>();
            fileView = reopenedFile.get();
            reopenedFile->replaceStableBytes(image);
            auto reopened = miare::testing::DatabaseAccess::open(
                std::move(reopenedFile),
                miare::EncryptionKeyView{encryptionKey},
                deterministicProviders(10 + transaction));
            require(reopened.hasValue());
            database.emplace(std::move(reopened).value());
            verify();
        }
    }
    database->close();
}

} // namespace

int main() {
    committedKeysSurviveMultipleTreeLevelsAndReopen();
    overflowValuesRemainAtomicAcrossReplacementAndDeletion();
    supersededStorageIsSafelyReused();
    postCreateGenerationsPublishAllocatorState();
    mutationsRewriteOnlyAffectedTreePaths();
    authenticatedExtentBoundariesRejectPhysicalTampering();
    randomizedHistoriesMatchAnIndependentReferenceModel();
}
