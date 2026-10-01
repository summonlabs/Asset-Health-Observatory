// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "fs_atomic.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace asset_health::detail::fs {
namespace {

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    std::string text;
    text.reserve(wide.size());
    for (const wchar_t character : wide) {
        text.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
    return text;
#else
    return path.string();
#endif
}

[[nodiscard]] Error io_error(const std::filesystem::path& path, const char* operation, std::uint32_t code) {
    return Error(ErrorCode::StoreIoError, std::string(operation) + " failed")
        .with_context("path", path_text(path))
        .with_context("system_error", std::to_string(code));
}

}  // namespace

struct ExclusiveLock::Handle {
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif
    bool exclusive = true;
};

ExclusiveLock::ExclusiveLock() noexcept = default;

ExclusiveLock::~ExclusiveLock() { release(); }

ExclusiveLock::ExclusiveLock(ExclusiveLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

ExclusiveLock& ExclusiveLock::operator=(ExclusiveLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool ExclusiveLock::held() const noexcept { return handle_ != nullptr; }

void ExclusiveLock::release() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    if (handle_->file != INVALID_HANDLE_VALUE) {
        // UnlockFileEx requires a pointer to the same OVERLAPPED region the lock
        // was taken with. The region here is "the whole file from offset zero",
        // so a zeroed structure names exactly it; a null pointer is not an
        // option and faults inside the kernel call.
        OVERLAPPED overlapped{};
        ::UnlockFileEx(handle_->file, 0, MAXDWORD, MAXDWORD, &overlapped);
        ::CloseHandle(handle_->file);
    }
#else
    if (handle_->descriptor >= 0) {
        ::flock(handle_->descriptor, LOCK_UN);
        ::close(handle_->descriptor);
    }
#endif
    delete handle_;
    handle_ = nullptr;
}

Outcome<ExclusiveLock> ExclusiveLock::acquire(const std::filesystem::path& file, bool exclusive) {
    auto* handle = new Handle();
    handle->exclusive = exclusive;
#if defined(_WIN32)
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    handle->file = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle->file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        delete handle;
        return io_error(file, "opening the lock file", code);
    }
    OVERLAPPED overlapped{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (exclusive) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    if (::LockFileEx(handle->file, flags, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(handle->file);
        delete handle;
        if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
            return Error(ErrorCode::StoreLocked,
                         "another process holds the write lock on this store")
                .with_context("path", path_text(file));
        }
        return io_error(file, "locking the lock file", code);
    }
#else
    handle->descriptor = ::open(file.c_str(), O_RDWR | O_CREAT, 0644);
    if (handle->descriptor < 0) {
        const int code = errno;
        delete handle;
        return io_error(file, "opening the lock file", static_cast<std::uint32_t>(code));
    }
    const int operation = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(handle->descriptor, operation) != 0) {
        const int code = errno;
        ::close(handle->descriptor);
        delete handle;
        if (code == EWOULDBLOCK || code == EAGAIN) {
            return Error(ErrorCode::StoreLocked, "another process holds the write lock on this store")
                .with_context("path", path_text(file));
        }
        return io_error(file, "locking the lock file", static_cast<std::uint32_t>(code));
    }
#endif
    ExclusiveLock lock;
    lock.handle_ = handle;
    return lock;
}

Status ensure_directory(const std::filesystem::path& directory) {
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (std::filesystem::is_directory(directory, error)) {
            return ok();
        }
        return Error(ErrorCode::StoreLayoutInvalid, "the store path exists and is not a directory")
            .with_context("path", path_text(directory));
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
        return io_error(directory, "creating the store directory", static_cast<std::uint32_t>(error.value()));
    }
    return ok();
}

Outcome<bool> file_exists(const std::filesystem::path& file) {
    std::error_code error;
    const bool present = std::filesystem::exists(file, error);
    if (error) {
        return io_error(file, "inspecting a file", static_cast<std::uint32_t>(error.value()));
    }
    if (!present) {
        return false;
    }
    const bool regular = std::filesystem::is_regular_file(file, error);
    return !error && regular;
}

Outcome<std::uint64_t> size_of_file(const std::filesystem::path& file) {
    const auto exists = file_exists(file);
    if (!exists.has_value()) {
        return exists.error();
    }
    if (!exists.value()) {
        return Error(ErrorCode::StoreNotFound, "the file does not exist").with_context("path", path_text(file));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) {
        return io_error(file, "reading the file size", static_cast<std::uint32_t>(error.value()));
    }
    return static_cast<std::uint64_t>(size);
}

Outcome<bool> read_file(const std::filesystem::path& file, std::uint64_t max_bytes,
                        std::vector<std::uint8_t>& out) {
    const auto exists = file_exists(file);
    if (!exists.has_value()) {
        return exists.error();
    }
    if (!exists.value()) {
        return false;
    }
    const auto size = size_of_file(file);
    if (!size.has_value()) {
        return size.error();
    }
    if (size.value() > max_bytes) {
        return Error(ErrorCode::PayloadTooLarge, "the file is larger than the configured bound")
            .with_context("path", path_text(file))
            .with_context("bytes", std::to_string(size.value()));
    }
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return io_error(file, "opening a file for reading", ::GetLastError());
    }
    out.assign(static_cast<std::size_t>(size.value()), 0);
    std::size_t filled = 0;
    while (filled < out.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - filled, 1u << 20));
        DWORD read = 0;
        if (::ReadFile(handle, out.data() + filled, chunk, &read, nullptr) == 0) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(handle);
            return io_error(file, "reading a file", code);
        }
        if (read == 0) {
            break;
        }
        filled += read;
    }
    ::CloseHandle(handle);
    if (filled != out.size()) {
        // The file shrank between the size query and the read. That is a torn
        // file, and it is reported as such rather than silently accepted short.
        out.resize(filled);
    }
#else
    const int descriptor = ::open(file.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return io_error(file, "opening a file for reading", static_cast<std::uint32_t>(errno));
    }
    out.assign(static_cast<std::size_t>(size.value()), 0);
    std::size_t filled = 0;
    while (filled < out.size()) {
        const ssize_t got = ::read(descriptor, out.data() + filled, out.size() - filled);
        if (got < 0) {
            const int code = errno;
            ::close(descriptor);
            return io_error(file, "reading a file", static_cast<std::uint32_t>(code));
        }
        if (got == 0) {
            break;
        }
        filled += static_cast<std::size_t>(got);
    }
    ::close(descriptor);
    out.resize(filled);
#endif
    return true;
}

namespace {

/// Writes to a unique temporary file beside the target, flushes it to the
/// device, and renames it over the target. \p replace selects whether an
/// existing target is replaced or the rename must fail.
[[nodiscard]] Status write_through(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes,
                                   bool replace) {
    std::filesystem::path temporary = target;
    temporary += replace ? ".tmp" : ".new";
    std::error_code error;
    std::filesystem::remove(temporary, error);
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return io_error(temporary, "creating a temporary file", ::GetLastError());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 20));
        DWORD count = 0;
        if (::WriteFile(handle, bytes.data() + written, chunk, &count, nullptr) == 0) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(handle);
            std::filesystem::remove(temporary, error);
            return io_error(temporary, "writing a file", code);
        }
        written += count;
    }
    if (::FlushFileBuffers(handle) == 0) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(handle);
        std::filesystem::remove(temporary, error);
        return io_error(temporary, "flushing a file", code);
    }
    ::CloseHandle(handle);
    const DWORD move_flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0u);
    if (::MoveFileExW(temporary.c_str(), target.c_str(), move_flags) == 0) {
        const DWORD code = ::GetLastError();
        std::filesystem::remove(temporary, error);
        if (!replace && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)) {
            return Error(ErrorCode::StorePublishFailed, "the target already exists and was not replaced")
                .with_context("path", path_text(target));
        }
        return io_error(target, "renaming a temporary file into place", code);
    }
#else
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return io_error(temporary, "creating a temporary file", static_cast<std::uint32_t>(errno));
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count <= 0) {
            const int code = errno;
            ::close(descriptor);
            std::filesystem::remove(temporary, error);
            return io_error(temporary, "writing a file", static_cast<std::uint32_t>(code));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const int code = errno;
        ::close(descriptor);
        std::filesystem::remove(temporary, error);
        return io_error(temporary, "flushing a file", static_cast<std::uint32_t>(code));
    }
    ::close(descriptor);
    if (!replace) {
        // link() then unlink() gives an atomic create-that-must-not-replace.
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            const int code = errno;
            std::filesystem::remove(temporary, error);
            if (code == EEXIST) {
                return Error(ErrorCode::StorePublishFailed, "the target already exists and was not replaced")
                    .with_context("path", path_text(target));
            }
            return io_error(target, "publishing a file", static_cast<std::uint32_t>(code));
        }
        std::filesystem::remove(temporary, error);
    } else if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int code = errno;
        std::filesystem::remove(temporary, error);
        return io_error(target, "renaming a temporary file into place", static_cast<std::uint32_t>(code));
    }
    // The directory entry itself must be durable for the rename to survive a
    // power loss, which is what makes the target's contents the commit point.
    const std::filesystem::path parent = target.parent_path();
    const int directory = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY);
    if (directory >= 0) {
        ::fsync(directory);
        ::close(directory);
    }
#endif
    return ok();
}

}  // namespace

Status write_atomic(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes) {
    return write_through(target, bytes, true);
}

Status write_new(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes) {
    return write_through(target, bytes, false);
}

Status remove_file(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::remove(file, error);
    if (error) {
        return io_error(file, "removing a file", static_cast<std::uint32_t>(error.value()));
    }
    return ok();
}

Outcome<std::vector<std::string>> list_files(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) {
            return io_error(directory, "inspecting the directory", static_cast<std::uint32_t>(error.value()));
        }
        return names;
    }
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return io_error(directory, "opening the directory", static_cast<std::uint32_t>(error.value()));
    }
    for (const auto& entry : iterator) {
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }
        names.push_back(entry.path().filename().string());
    }
    return names;
}

}  // namespace asset_health::detail::fs
