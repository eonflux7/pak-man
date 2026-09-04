#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pakman {

enum class ArchiveType { stored, compressed };

struct Entry {
    std::string path_utf8;
    std::vector<std::uint8_t> path_bytes;
    std::uint32_t logical_offset{};
    std::uint32_t original_size{};
    std::uint64_t filetime_ticks{};
    std::uint64_t physical_offset{};
    std::uint64_t stored_size{};
};

struct VerifyReport {
    std::uint64_t original_bytes{};
    std::uint64_t stored_bytes{};
    std::uint64_t compressed_blocks{};
    std::vector<std::string> warnings;
};

using Progress = std::function<bool(std::uint64_t done, std::uint64_t total,
                                    std::string_view current)>;

class Archive {
public:
    static Archive open(const std::filesystem::path& path);
    const std::filesystem::path& path() const noexcept { return path_; }
    ArchiveType type() const noexcept { return type_; }
    std::uint32_t version() const noexcept { return version_; }
    std::uint32_t platform() const noexcept { return platform_; }
    std::span<const Entry> entries() const noexcept { return entries_; }
    VerifyReport verify(bool full = true, Progress progress = {}) const;
    void extract(const std::filesystem::path& destination,
                 std::span<const std::size_t> selection = {},
                 bool overwrite = false, Progress progress = {}) const;

private:
    std::filesystem::path path_;
    ArchiveType type_{};
    std::uint32_t version_{};
    std::uint32_t platform_{};
    std::uint64_t data_base_{};
    std::vector<Entry> entries_;
};

struct CreateOptions {
    ArchiveType type{ArchiveType::stored};
    bool deterministic{true};
};

void create_archive(const std::filesystem::path& source_directory,
                    const std::filesystem::path& destination,
                    const CreateOptions& options = {}, Progress progress = {});

std::string type_name(ArchiveType type);

} // namespace pakman

