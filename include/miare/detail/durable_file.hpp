#pragma once

#include <miare/error.hpp>
#include <miare/types.hpp>

#include <algorithm>
#include <cerrno>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace miare::detail {

struct NativeFileIdentity {
    std::uint64_t filesystem;
    std::uint64_t file;

    friend auto operator<=>(
        const NativeFileIdentity&,
        const NativeFileIdentity&) noexcept = default;
};

class OpenSessionRegistry {
public:
    static void acquire(NativeFileIdentity identity) {
        std::lock_guard lock{mutex()};
        if (!identities().insert(identity).second) {
            throw DatabaseError{Errc::InUse, "database file is already in use"};
        }
    }

    static void release(NativeFileIdentity identity) noexcept {
        try {
            std::lock_guard lock{mutex()};
            identities().erase(identity);
        } catch (...) {
        }
    }

private:
    [[nodiscard]] static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }

    [[nodiscard]] static std::set<NativeFileIdentity>& identities() {
        static std::set<NativeFileIdentity> instance;
        return instance;
    }
};

class DurableFile {
public:
    DurableFile() = default;
    DurableFile(const DurableFile&) = delete;
    DurableFile& operator=(const DurableFile&) = delete;
    virtual ~DurableFile() = default;

    [[nodiscard]] virtual std::uint64_t size() const = 0;
    virtual void readExactAt(std::uint64_t offset, MutableByteView destination) = 0;
    virtual void writeExactAt(std::uint64_t offset, ByteView source) = 0;
    virtual void resize(std::uint64_t length) = 0;
    virtual void stableStorageBarrier() = 0;
};

class NativeDurableFile final : public DurableFile {
public:
    [[nodiscard]] static std::unique_ptr<NativeDurableFile> createNew(
        const std::filesystem::path& path) {
        return openPath(path, true);
    }

    [[nodiscard]] static std::unique_ptr<NativeDurableFile> openExisting(
        const std::filesystem::path& path) {
        return openPath(path, false);
    }

    NativeDurableFile(const NativeDurableFile&) = delete;
    NativeDurableFile& operator=(const NativeDurableFile&) = delete;

    [[nodiscard]] std::uint64_t size() const override {
#ifdef _WIN32
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle_, &length)) {
            throwWindows(Errc::Io, "database file inspection failed");
        }
        return static_cast<std::uint64_t>(length.QuadPart);
#else
        struct stat status {};
        if (::fstat(descriptor_, &status) != 0) {
            throwPosix(Errc::Io, "database file inspection failed");
        }
        return static_cast<std::uint64_t>(status.st_size);
#endif
    }

    static void installExclusive(
        const std::filesystem::path& temporary,
        const std::filesystem::path& target) {
#ifdef _WIN32
        if (!MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            throwWindows(Errc::Io, "database file installation failed");
        }
#else
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            throwPosix(Errc::Io, "database file installation failed");
        }
        if (::unlink(temporary.c_str()) != 0) {
            throwPosix(Errc::Io, "database temporary-file removal failed");
        }
        stabilizeParent(target);
#endif
    }

    ~NativeDurableFile() override {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#endif
        if (identity_) {
            OpenSessionRegistry::release(*identity_);
        }
    }

    void readExactAt(std::uint64_t offset, MutableByteView destination) override {
        validateRange(offset, destination.size());
        std::size_t transferred = 0;
        while (transferred != destination.size()) {
#ifdef _WIN32
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                destination.size() - transferred,
                std::numeric_limits<DWORD>::max()));
            OVERLAPPED position{};
            const auto current = offset + transferred;
            position.Offset = static_cast<DWORD>(current);
            position.OffsetHigh = static_cast<DWORD>(current >> 32U);
            position.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (position.hEvent == nullptr) {
                throwWindows(Errc::Io, "positioned read event creation failed");
            }
            DWORD count = 0;
            const BOOL started = ReadFile(
                handle_, destination.data() + transferred, chunk, nullptr, &position);
            const auto startError = started ? ERROR_SUCCESS : GetLastError();
            if (!started && startError != ERROR_IO_PENDING) {
                CloseHandle(position.hEvent);
                SetLastError(startError);
                throwWindows(Errc::Io, "positioned read failed");
            }
            if (!GetOverlappedResult(handle_, &position, &count, TRUE)) {
                const auto error = GetLastError();
                CloseHandle(position.hEvent);
                SetLastError(error);
                throwWindows(Errc::Io, "positioned read failed");
            }
            CloseHandle(position.hEvent);
            if (count == 0) {
                throw DatabaseError{Errc::Io, "unexpected end of database file"};
            }
#else
            const auto count = ::pread(
                descriptor_,
                destination.data() + transferred,
                destination.size() - transferred,
                static_cast<off_t>(offset + transferred));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throwPosix(Errc::Io, "positioned read failed");
            }
            if (count == 0) {
                throw DatabaseError{Errc::Io, "unexpected end of database file"};
            }
#endif
            transferred += static_cast<std::size_t>(count);
        }
    }

    void writeExactAt(std::uint64_t offset, ByteView source) override {
        validateRange(offset, source.size());
        std::size_t transferred = 0;
        while (transferred != source.size()) {
#ifdef _WIN32
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                source.size() - transferred,
                std::numeric_limits<DWORD>::max()));
            OVERLAPPED position{};
            const auto current = offset + transferred;
            position.Offset = static_cast<DWORD>(current);
            position.OffsetHigh = static_cast<DWORD>(current >> 32U);
            position.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (position.hEvent == nullptr) {
                throwWindows(Errc::Io, "positioned write event creation failed");
            }
            DWORD count = 0;
            const BOOL started = WriteFile(
                handle_, source.data() + transferred, chunk, nullptr, &position);
            const auto startError = started ? ERROR_SUCCESS : GetLastError();
            if (!started && startError != ERROR_IO_PENDING) {
                CloseHandle(position.hEvent);
                SetLastError(startError);
                throwWindows(Errc::Io, "positioned write failed");
            }
            if (!GetOverlappedResult(handle_, &position, &count, TRUE)) {
                const auto error = GetLastError();
                CloseHandle(position.hEvent);
                SetLastError(error);
                throwWindows(Errc::Io, "positioned write failed");
            }
            CloseHandle(position.hEvent);
            if (count == 0) {
                throw DatabaseError{Errc::Io, "positioned write made no progress"};
            }
#else
            const auto count = ::pwrite(
                descriptor_,
                source.data() + transferred,
                source.size() - transferred,
                static_cast<off_t>(offset + transferred));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throwPosix(Errc::Io, "positioned write failed");
            }
            if (count == 0) {
                throw DatabaseError{Errc::Io, "positioned write made no progress"};
            }
#endif
            transferred += static_cast<std::size_t>(count);
        }
    }

    void resize(std::uint64_t length) override {
        validateRange(length, 0);
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(length);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(handle_)) {
            throwWindows(Errc::Io, "database file resize failed");
        }
#else
        if (::ftruncate(descriptor_, static_cast<off_t>(length)) != 0) {
            throwPosix(Errc::Io, "database file resize failed");
        }
#endif
    }

    void stableStorageBarrier() override {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) {
            throwWindows(Errc::Durability, "stable-storage barrier failed");
        }
#elif defined(__APPLE__)
        if (::fcntl(descriptor_, F_FULLFSYNC) != 0) {
            throwPosix(Errc::Durability, "stable-storage barrier failed");
        }
#else
        if (::fsync(descriptor_) != 0) {
            throwPosix(Errc::Durability, "stable-storage barrier failed");
        }
#endif
    }

private:
#ifdef _WIN32
    explicit NativeDurableFile(HANDLE handle) noexcept : handle_(handle) {}
#else
    explicit NativeDurableFile(int descriptor) noexcept : descriptor_(descriptor) {}
#endif

    void registerOpenSession() {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle_, &information)) {
            throwWindows(Errc::Io, "database file identity inspection failed");
        }
        const NativeFileIdentity identity{
            information.dwVolumeSerialNumber,
            (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
                information.nFileIndexLow};
#else
        struct stat status {};
        if (::fstat(descriptor_, &status) != 0) {
            throwPosix(Errc::Io, "database file identity inspection failed");
        }
        const NativeFileIdentity identity{
            static_cast<std::uint64_t>(status.st_dev),
            static_cast<std::uint64_t>(status.st_ino)};
#endif
        OpenSessionRegistry::acquire(identity);
        identity_ = identity;
    }

    static void validateRange(std::uint64_t offset, std::size_t length) {
        constexpr auto maxOffset = static_cast<std::uint64_t>(
#ifdef _WIN32
            std::numeric_limits<LONGLONG>::max()
#else
            std::numeric_limits<off_t>::max()
#endif
        );
        if (offset > maxOffset || length > maxOffset - offset) {
            throw ContractError{Errc::InvalidArgument, "file range is not representable"};
        }
    }

    [[nodiscard]] static std::unique_ptr<NativeDurableFile> openPath(
        const std::filesystem::path& path,
        bool create) {
#ifdef _WIN32
        const DWORD disposition = create ? CREATE_NEW : OPEN_EXISTING;
        HANDLE handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            disposition,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (!create && (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)) {
                throw DatabaseError{
                    Errc::InUse,
                    "database file is already in use",
                    std::error_code{static_cast<int>(error), std::system_category()}};
            }
            throw DatabaseError{
                Errc::Io,
                "database file open failed",
                std::error_code{static_cast<int>(error), std::system_category()}};
        }
        if (GetFileType(handle) != FILE_TYPE_DISK) {
            CloseHandle(handle);
            throw DatabaseError{Errc::Io, "database path is not a local regular file"};
        }
        auto file = std::unique_ptr<NativeDurableFile>{new NativeDurableFile{handle}};
        file->registerOpenSession();
        return file;
#else
        const int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0);
        const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            throwPosix(Errc::Io, "database file open failed");
        }

        struct stat status {};
        if (::fstat(descriptor, &status) != 0) {
            const int saved = errno;
            ::close(descriptor);
            errno = saved;
            throwPosix(Errc::Io, "database file inspection failed");
        }
        if (!S_ISREG(status.st_mode)) {
            ::close(descriptor);
            errno = EINVAL;
            throwPosix(Errc::Io, "database path is not a local regular file");
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int saved = errno;
            ::close(descriptor);
            if (saved == EWOULDBLOCK || saved == EAGAIN) {
                throw DatabaseError{
                    Errc::InUse,
                    "database file is already in use",
                    std::error_code{saved, std::generic_category()}};
            }
            errno = saved;
            throwPosix(Errc::Io, "database file lock failed");
        }
        auto file = std::unique_ptr<NativeDurableFile>{new NativeDurableFile{descriptor}};
        file->registerOpenSession();
        return file;
#endif
    }

#ifndef _WIN32
    static void stabilizeParent(const std::filesystem::path& path) {
        auto parent = path.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        const int descriptor = ::open(
            parent.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
            | O_DIRECTORY
#endif
        );
        if (descriptor < 0) {
            throwPosix(Errc::Durability, "database directory open failed");
        }
        if (::fsync(descriptor) != 0) {
            const int saved = errno;
            ::close(descriptor);
            errno = saved;
            throwPosix(Errc::Durability, "database directory barrier failed");
        }
        if (::close(descriptor) != 0) {
            throwPosix(Errc::Io, "database directory close failed");
        }
    }
#endif

#ifdef _WIN32
    [[noreturn]] static void throwWindows(Errc code, const char* message) {
        const auto error = GetLastError();
        throw DatabaseError{
            code,
            message,
            std::error_code{static_cast<int>(error), std::system_category()}};
    }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    [[noreturn]] static void throwPosix(Errc code, const char* message) {
        throw DatabaseError{
            code,
            message,
            std::error_code{errno, std::generic_category()}};
    }
    int descriptor_ = -1;
#endif
    std::optional<NativeFileIdentity> identity_;
};

} // namespace miare::detail
