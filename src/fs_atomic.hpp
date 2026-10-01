// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// File primitives: the smallest set of operating-system operations this store
// needs, each one wrapped so the rest of the code never sees a raw handle, a
// partially written file, or an unflushed buffer.
//
// There is no third-party dependency here. The Windows implementation uses
// CreateFileW, WriteFile, FlushFileBuffers, MoveFileExW and LockFileEx; the
// POSIX implementation uses open, write, fsync, rename and flock.

#ifndef ASSET_HEALTH_SRC_FS_ATOMIC_HPP
#define ASSET_HEALTH_SRC_FS_ATOMIC_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "asset_health/error.hpp"

namespace asset_health::detail::fs {

/// Creates a directory and every missing parent. Succeeds when it exists.
[[nodiscard]] Status ensure_directory(const std::filesystem::path& directory);

/// True when the path names an existing regular file.
[[nodiscard]] Outcome<bool> file_exists(const std::filesystem::path& file);

/// Reads a whole file. Refuses a file larger than \p max_bytes before allocating
/// for it. Reports found == false when the file does not exist.
[[nodiscard]] Outcome<bool> read_file(const std::filesystem::path& file, std::uint64_t max_bytes,
                                      std::vector<std::uint8_t>& out);

/// Size of a file, as a rejection when it does not exist. Named size_of_file
/// rather than file_size so that argument-dependent lookup cannot reach
/// std::filesystem::file_size, which has the same signature and a different
/// failure model.
[[nodiscard]] Outcome<std::uint64_t> size_of_file(const std::filesystem::path& file);

/// Writes \p bytes to \p target atomically: the bytes go to a temporary file in
/// the same directory, are flushed to the device, and the temporary file is then
/// renamed over the target. A reader therefore sees either the previous contents
/// or the new contents, never a mixture, and a crash leaves one or the other.
[[nodiscard]] Status write_atomic(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes);

/// Writes a file that must not already exist. The rename fails rather than
/// replacing an existing generation.
[[nodiscard]] Status write_new(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes);

/// Removes a file. Succeeds when it is already absent.
[[nodiscard]] Status remove_file(const std::filesystem::path& file);

/// Names of the regular files directly inside \p directory, unsorted. An absent
/// directory yields an empty list.
[[nodiscard]] Outcome<std::vector<std::string>> list_files(const std::filesystem::path& directory);

/// A kernel-enforced exclusive lock held on a file for the lifetime of the
/// object. A second attempt to take the same lock, in this or another process,
/// fails immediately with StoreLocked rather than waiting.
class ExclusiveLock {
public:
    ExclusiveLock() noexcept;
    ~ExclusiveLock();
    ExclusiveLock(ExclusiveLock&& other) noexcept;
    ExclusiveLock& operator=(ExclusiveLock&& other) noexcept;
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

    [[nodiscard]] static Outcome<ExclusiveLock> acquire(const std::filesystem::path& file, bool exclusive);

    [[nodiscard]] bool held() const noexcept;
    void release() noexcept;

private:
    struct Handle;
    Handle* handle_ = nullptr;
};

}  // namespace asset_health::detail::fs

#endif  // ASSET_HEALTH_SRC_FS_ATOMIC_HPP
