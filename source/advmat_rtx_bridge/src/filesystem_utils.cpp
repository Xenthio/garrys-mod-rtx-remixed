#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace advmat::rtx_bridge::detail {
namespace {

std::atomic<std::uint64_t> g_tempCounter{0};

std::uint64_t ProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

bool FlushFileToDisk(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "CreateFileW for durable staging failed with Win32 error " +
            std::to_string(GetLastError());
        return false;
    }
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const DWORD flushError = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        error = "FlushFileBuffers failed with Win32 error " +
            std::to_string(flushError);
        return false;
    }
#else
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        error = "cannot open staging file for durable flush";
        return false;
    }
    const bool flushed = fsync(descriptor) == 0;
    close(descriptor);
    if (!flushed) {
        error = "fsync failed for staging file";
        return false;
    }
#endif
    return true;
}

} // namespace

bool IsDescendantOrSame(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
    const auto normalizedCandidate = candidate.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto candidateIt = normalizedCandidate.begin();
    auto rootIt = normalizedRoot.begin();
    for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == normalizedCandidate.end()) {
            return false;
        }
#ifdef _WIN32
        auto candidatePart = candidateIt->wstring();
        auto rootPart = rootIt->wstring();
        std::transform(candidatePart.begin(), candidatePart.end(), candidatePart.begin(),
                       [](wchar_t character) { return std::towlower(character); });
        std::transform(rootPart.begin(), rootPart.end(), rootPart.begin(),
                       [](wchar_t character) { return std::towlower(character); });
        if (candidatePart != rootPart) return false;
#else
        if (*candidateIt != *rootIt) return false;
#endif
    }
    return true;
}

bool InspectPlainPath(const std::filesystem::path& path, bool requireDirectory,
                      bool& exists, std::string& error) {
    exists = false;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) return true;
        error = "cannot inspect owned path: " + ec.message();
        return false;
    }
    if (!std::filesystem::exists(status)) return true;
    if (std::filesystem::is_symlink(status)) {
        error = "owned path is a symbolic link or reparse point";
        return false;
    }
#ifdef _WIN32
    // MSVC's filesystem status has varied in how it reports junctions. The
    // native attribute closes that gap for every existing destination
    // component and leaf, not just the final quarantine directory.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "cannot inspect owned path attributes (Win32 error " +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "owned path is a symbolic link or reparse point";
        return false;
    }
#endif
    if (requireDirectory ? !std::filesystem::is_directory(status) :
                           !std::filesystem::is_regular_file(status)) {
        error = requireDirectory ? "owned path is not a directory" :
                                   "owned path is not a regular file";
        return false;
    }
    exists = true;
    return true;
}

namespace {

bool PlainDirectoryTree(const std::filesystem::path& root,
                        const std::filesystem::path& directory,
                        bool createMissing, std::string& error) {
    const auto normalizedRoot = root.lexically_normal();
    const auto normalizedDirectory = directory.lexically_normal();
    if (normalizedRoot.empty() || normalizedDirectory.empty() ||
        !normalizedRoot.is_absolute() || !normalizedDirectory.is_absolute() ||
        !IsDescendantOrSame(normalizedDirectory, normalizedRoot)) {
        error = "owned directory is outside the validated filesystem root";
        return false;
    }

    bool rootExists = false;
    if (!InspectPlainPath(normalizedRoot, true, rootExists, error) || !rootExists) {
        if (error.empty()) error = "validated filesystem root is missing";
        return false;
    }

    std::vector<std::filesystem::path> components;
    auto current = normalizedRoot;
    const auto relative = normalizedDirectory.lexically_relative(normalizedRoot);
    if (relative.empty() && normalizedDirectory != normalizedRoot) {
        error = "owned directory cannot be made relative to the validated root";
        return false;
    }
    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.has_root_path()) {
            error = "owned directory contains an unsafe path component";
            return false;
        }
        current /= part;
        components.push_back(current);

        bool exists = false;
        if (!InspectPlainPath(current, true, exists, error)) return false;
        if (!exists) {
            if (!createMissing) {
                error = "owned directory component is missing";
                return false;
            }
            std::error_code ec;
            // Create exactly one component. If another actor wins the race,
            // reinspection below accepts only a real non-reparse directory.
            std::filesystem::create_directory(current, ec);
            if (ec && ec != std::errc::file_exists) {
                error = "cannot create owned directory component: " + ec.message();
                return false;
            }
            if (!InspectPlainPath(current, true, exists, error) || !exists) {
                if (error.empty()) error = "created owned directory component is missing";
                return false;
            }
        }
    }

    // Revalidate the complete chain after the final creation. Callers invoke
    // this again immediately before writes/renames, narrowing replacement races
    // without ever trusting create_directories through an existing junction.
    for (const auto& component : components) {
        bool exists = false;
        if (!InspectPlainPath(component, true, exists, error) || !exists) {
            if (error.empty()) error = "owned directory component disappeared";
            return false;
        }
    }
    return true;
}

bool RemovePlainDirectoryTreeRecursive(const std::filesystem::path& directory,
                                       std::string& error) {
    bool directoryExists = false;
    if (!InspectPlainPath(directory, true, directoryExists, error)) return false;
    if (!directoryExists) return true;

    // Do not hand a whole tree to remove_all: on Windows, a junction nested
    // below an otherwise ordinary directory has historically been reported as
    // a directory by some filesystem implementations. Enumerate without the
    // follow-symlink option and validate every child's native attributes before
    // either descending or deleting it.
    std::vector<std::filesystem::path> children;
    std::error_code ec;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::none, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        error = "cannot enumerate retirement directory: " + ec.message();
        return false;
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            error = "cannot enumerate retirement directory: " + ec.message();
            return false;
        }
        children.push_back(iterator->path());
    }
    if (ec) {
        error = "cannot enumerate retirement directory: " + ec.message();
        return false;
    }

    for (const auto& child : children) {
        const auto status = std::filesystem::symlink_status(child, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                ec.clear();
                continue;
            }
            error = "cannot inspect retirement entry: " + ec.message();
            return false;
        }
        if (!std::filesystem::exists(status)) continue;
        if (std::filesystem::is_symlink(status)) {
            error = "retirement tree contains a symbolic link or reparse point";
            return false;
        }
        if (std::filesystem::is_directory(status)) {
            if (!RemovePlainDirectoryTreeRecursive(child, error)) return false;
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            error = "retirement tree contains a non-regular filesystem entry";
            return false;
        }

        bool childExists = false;
        if (!InspectPlainPath(child, false, childExists, error)) return false;
        if (!childExists) continue;
        std::filesystem::remove(child, ec);
        if (ec) {
            error = "cannot remove retired file: " + ec.message();
            return false;
        }
    }

    // A raced-in child makes this fail closed with directory-not-empty. A
    // raced-in reparse point is removed as a link by remove rather than walked;
    // the target is never recursively traversed by this routine.
    if (!InspectPlainPath(directory, true, directoryExists, error)) return false;
    if (!directoryExists) return true;
    std::filesystem::remove(directory, ec);
    if (ec) {
        error = "cannot remove retired directory: " + ec.message();
        return false;
    }
    return true;
}

bool SameOwnedPath(const std::filesystem::path& left,
                   const std::filesystem::path& right) {
    return IsDescendantOrSame(left, right) && IsDescendantOrSame(right, left);
}

std::filesystem::path OwnedPathKey(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal();
#ifdef _WIN32
    auto native = normalized.native();
    std::transform(native.begin(), native.end(), native.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    normalized = std::filesystem::path(native);
#endif
    return normalized;
}

bool PrunePlainDirectoryTreeRecursive(
        const std::filesystem::path& directory,
        const std::vector<std::filesystem::path>& keepKeys,
        bool removeWhenEmpty, bool& remains, std::string& error) {
    remains = false;
    bool directoryExists = false;
    if (!InspectPlainPath(directory, true, directoryExists, error)) return false;
    if (!directoryExists) return true;

    std::vector<std::filesystem::path> children;
    std::error_code ec;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::none, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        error = "cannot enumerate owned cache directory: " + ec.message();
        return false;
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            error = "cannot enumerate owned cache directory: " + ec.message();
            return false;
        }
        children.push_back(iterator->path());
    }
    if (ec) {
        error = "cannot enumerate owned cache directory: " + ec.message();
        return false;
    }

    bool hasRemainingChild = false;
    for (const auto& child : children) {
        const auto status = std::filesystem::symlink_status(child, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                ec.clear();
                continue;
            }
            error = "cannot inspect owned cache entry: " + ec.message();
            return false;
        }
        if (!std::filesystem::exists(status)) continue;
        if (std::filesystem::is_symlink(status)) {
            error = "owned cache tree contains a symbolic link or reparse point";
            return false;
        }
        if (std::filesystem::is_directory(status)) {
            bool childRemains = false;
            if (!PrunePlainDirectoryTreeRecursive(
                    child, keepKeys, true, childRemains, error)) {
                return false;
            }
            hasRemainingChild = hasRemainingChild || childRemains;
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            error = "owned cache tree contains a non-regular filesystem entry";
            return false;
        }

        bool childExists = false;
        if (!InspectPlainPath(child, false, childExists, error)) return false;
        if (!childExists) continue;
        const bool keep = std::binary_search(
            keepKeys.begin(), keepKeys.end(), OwnedPathKey(child));
        if (keep) {
            hasRemainingChild = true;
            continue;
        }
        std::filesystem::remove(child, ec);
        if (ec) {
            error = "cannot remove inactive owned cache file: " + ec.message();
            return false;
        }
    }

    if (!removeWhenEmpty || hasRemainingChild) {
        remains = true;
        return true;
    }
    if (!InspectPlainPath(directory, true, directoryExists, error)) return false;
    if (!directoryExists) return true;
    std::filesystem::remove(directory, ec);
    if (ec) {
        error = "cannot remove empty owned cache directory: " + ec.message();
        return false;
    }
    if (!std::filesystem::exists(directory, ec) && !ec) return true;
    if (ec) {
        error = "cannot verify owned cache directory removal: " + ec.message();
        return false;
    }
    error = "owned cache directory changed during pruning";
    return false;
}

} // namespace

bool ValidatePlainDirectoryTree(const std::filesystem::path& root,
                                const std::filesystem::path& directory,
                                std::string& error) {
    return PlainDirectoryTree(root, directory, false, error);
}

bool EnsurePlainDirectoryTree(const std::filesystem::path& root,
                              const std::filesystem::path& directory,
                              std::string& error) {
    return PlainDirectoryTree(root, directory, true, error);
}

bool RemovePlainDirectoryTree(const std::filesystem::path& directory,
                              std::string& error) {
    return RemovePlainDirectoryTreeRecursive(directory, error);
}

bool PrunePlainDirectoryTree(const std::filesystem::path& directory,
                             const std::vector<std::filesystem::path>& keepFiles,
                             std::string& error) {
    std::vector<std::filesystem::path> keepKeys;
    keepKeys.reserve(keepFiles.size());
    for (const auto& keep : keepFiles) {
        if (SameOwnedPath(keep, directory) || !IsDescendantOrSame(keep, directory)) {
            error = "owned cache keep path escaped its directory";
            return false;
        }
        keepKeys.push_back(OwnedPathKey(keep));
    }
    std::sort(keepKeys.begin(), keepKeys.end());
    keepKeys.erase(std::unique(keepKeys.begin(), keepKeys.end()), keepKeys.end());
    bool remains = false;
    return PrunePlainDirectoryTreeRecursive(
        directory, keepKeys, false, remains, error);
}

std::filesystem::path MakeTemporarySibling(const std::filesystem::path& destination) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sequence = g_tempCounter.fetch_add(1, std::memory_order_relaxed);
    return destination.parent_path() /
        (destination.filename().string() + ".tmp." + std::to_string(ProcessId()) + "." +
         std::to_string(now) + "." + std::to_string(sequence));
}

bool AtomicReplacePath(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "MoveFileExW failed with Win32 error " + std::to_string(GetLastError());
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(source, destination, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
#endif
    return true;
}

bool AtomicWriteBytes(const std::filesystem::path& destination,
                      const std::vector<std::uint8_t>& contents,
                      std::string& error) {
    bool parentExists = false;
    if (!InspectPlainPath(destination.parent_path(), true, parentExists, error) ||
        !parentExists) {
        if (error.empty()) error = "destination directory is missing";
        return false;
    }
    bool destinationExists = false;
    if (!InspectPlainPath(destination, false, destinationExists, error)) {
        return false;
    }

    const auto staging = MakeTemporarySibling(destination);
    bool stagingExists = false;
    if (!InspectPlainPath(staging, false, stagingExists, error)) return false;
    if (stagingExists) {
        error = "staging path collision";
        return false;
    }
    std::error_code ec;
    {
        std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot open staging file for writing";
            return false;
        }
        if (!contents.empty()) {
            stream.write(reinterpret_cast<const char*>(contents.data()),
                         static_cast<std::streamsize>(contents.size()));
        }
        stream.flush();
        if (!stream) {
            error = "failed while writing staging file";
            stream.close();
            std::filesystem::remove(staging, ec);
            return false;
        }
    }

    // The immutable dependency (or new root ledger) must reach stable storage
    // before its atomic rename is allowed to publish it. This preserves the
    // dependency-before-root ordering across a process or machine crash.
    if (!FlushFileToDisk(staging, error)) {
        std::filesystem::remove(staging, ec);
        return false;
    }

    // Recheck both the exact parent and destination leaf immediately before
    // publication. Replacing a regular file is intended; following or
    // preserving a newly planted reparse point is not.
    if (!InspectPlainPath(destination.parent_path(), true, parentExists, error) ||
        !parentExists || !InspectPlainPath(destination, false, destinationExists, error)) {
        if (error.empty()) error = "destination path changed before publication";
        std::filesystem::remove(staging, ec);
        return false;
    }

    if (!AtomicReplacePath(staging, destination, error)) {
        std::filesystem::remove(staging, ec);
        return false;
    }
    return true;
}

bool AtomicWriteText(const std::filesystem::path& destination,
                     const std::string& contents,
                     std::string& error) {
    return AtomicWriteBytes(destination,
        std::vector<std::uint8_t>(contents.begin(), contents.end()), error);
}

} // namespace advmat::rtx_bridge::detail
