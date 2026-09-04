#include "pakman/archive.hpp"

#include <Windows.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace pakman {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

template<class T> T read_le(std::istream& in) {
    std::array<unsigned char, sizeof(T)> b{};
    if (!in.read(reinterpret_cast<char*>(b.data()), b.size())) fail("Unexpected end of archive");
    T value{};
    for (std::size_t i = 0; i < b.size(); ++i) value |= static_cast<T>(b[i]) << (i * 8);
    return value;
}

template<class T> void write_le(std::ostream& out, T value) {
    std::array<unsigned char, sizeof(T)> b{};
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<unsigned char>(value >> (i * 8));
    out.write(reinterpret_cast<const char*>(b.data()), b.size());
    if (!out) fail("Failed writing archive");
}

std::string win_error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring w = text ? text : L"Windows error";
    if (text) LocalFree(text);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), result.data(), n, nullptr, nullptr);
    return result;
}

std::string cp1252_to_utf8(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    int wn = MultiByteToWideChar(1252, 0, reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<int>(bytes.size()), nullptr, 0);
    if (!wn) fail("Cannot decode Windows-1252 archive path");
    std::wstring wide(wn, L'\0');
    MultiByteToWideChar(1252, 0, reinterpret_cast<const char*>(bytes.data()),
                        static_cast<int>(bytes.size()), wide.data(), wn);
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wn, out.data(), n, nullptr, nullptr);
    return out;
}

std::string path_to_utf8(const fs::path& path) {
    auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::vector<std::uint8_t> utf8_to_cp1252(std::string_view text) {
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!wn) fail("Invalid UTF-8 file name");
    std::wstring wide(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), wn);
    BOOL fallback = FALSE;
    int n = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, wide.data(), wn, nullptr, 0, nullptr, &fallback);
    if (!n || fallback) fail("A file name cannot be represented in Windows-1252: " + std::string(text));
    std::vector<std::uint8_t> out(n);
    WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, wide.data(), wn,
                        reinterpret_cast<char*>(out.data()), n, nullptr, &fallback);
    return out;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool reserved_component(std::string component) {
    while (!component.empty() && (component.back() == '.' || component.back() == ' ')) component.pop_back();
    auto dot = component.find('.');
    component = lower_ascii(component.substr(0, dot));
    static const std::set<std::string> fixed{"con", "prn", "aux", "nul"};
    if (fixed.contains(component)) return true;
    for (auto prefix : {"com", "lpt"}) {
        if (component.size() == 4 && component.starts_with(prefix) && component[3] >= '1' && component[3] <= '9') return true;
    }
    return false;
}

fs::path safe_relative(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() == '/' || (path.size() >= 2 && path[1] == ':')) fail("Unsafe archive path: " + path);
    fs::path result;
    std::stringstream ss(path);
    std::string component;
    while (std::getline(ss, component, '/')) {
        if (component.empty() || component == "." || component == ".." || component.find(':') != std::string::npos ||
            component.back() == '.' || component.back() == ' ' || reserved_component(component))
            fail("Unsafe archive path: " + path);
        auto u8 = std::u8string(reinterpret_cast<const char8_t*>(component.data()), component.size());
        result /= fs::path(u8);
    }
    return result;
}

std::uint64_t filetime_for(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        fail("Cannot read file timestamp: " + win_error(GetLastError()));
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

void set_filetime(const fs::path& path, std::uint64_t ticks) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

std::vector<unsigned char> inflate_block(std::span<const unsigned char> input, std::uint32_t expected) {
    std::vector<unsigned char> output(expected);
    uLongf size = expected;
    uLong source_size = static_cast<uLong>(input.size());
    int rc = uncompress2(output.data(), &size, input.data(), &source_size);
    if (rc != Z_OK || size != expected) fail("Invalid zlib block or original block length");
    return output;
}

std::vector<unsigned char> deflate_block(std::span<const unsigned char> input) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 13, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        fail("Could not initialize zlib");
    std::vector<unsigned char> out(deflateBound(&zs, static_cast<uLong>(input.size())));
    zs.next_in = const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&zs); fail("zlib compression failed"); }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

std::uint32_t stream_entry(const ArchiveType type, std::ifstream& in, const Entry& e,
                           std::ostream* output, bool validate_only, std::uint64_t& blocks) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(e.physical_offset));
    if (!in) fail("Cannot seek to archive entry");
    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t produced = 0;
    if (type == ArchiveType::stored) {
        std::array<unsigned char, 64 * 1024> buffer{};
        while (produced < e.original_size) {
            auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), e.original_size - produced));
            if (!in.read(reinterpret_cast<char*>(buffer.data()), n)) fail("Truncated stored entry");
            if (output) output->write(reinterpret_cast<char*>(buffer.data()), n);
            crc = crc32(crc, buffer.data(), static_cast<uInt>(n));
            produced += n;
        }
    } else {
        while (true) {
            auto compressed = read_le<std::uint32_t>(in);
            if (!compressed) break;
            auto original = read_le<std::uint32_t>(in);
            if (original > 4096 || !original || compressed > e.stored_size) fail("Invalid compressed block size");
            std::vector<unsigned char> packed(compressed);
            if (!in.read(reinterpret_cast<char*>(packed.data()), packed.size())) fail("Truncated compressed block");
            auto raw = inflate_block(packed, original);
            if (output) output->write(reinterpret_cast<char*>(raw.data()), raw.size());
            crc = crc32(crc, raw.data(), static_cast<uInt>(raw.size()));
            produced += raw.size();
            ++blocks;
            if (produced > e.original_size) fail("Compressed entry exceeds declared length");
        }
        if (produced != e.original_size) fail("Compressed entry length mismatch");
        auto end = static_cast<std::uint64_t>(in.tellg());
        if (end != e.physical_offset + e.stored_size) fail("Compressed entry trailer does not match next offset");
    }
    if (output && !*output) fail("Failed writing extracted file");
    (void)validate_only;
    return static_cast<std::uint32_t>(crc);
}

} // namespace

Archive Archive::open(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("Cannot open archive");
    char magic[4]{};
    if (!in.read(magic, 4) || std::string_view(magic, 3) != "PAK" || (magic[3] != 'A' && magic[3] != 'C'))
        fail("Not a Commandos: Strike Force PAK archive");
    Archive a;
    a.path_ = path;
    a.type_ = magic[3] == 'A' ? ArchiveType::stored : ArchiveType::compressed;
    a.version_ = read_le<std::uint32_t>(in);
    a.platform_ = read_le<std::uint32_t>(in);
    auto count = read_le<std::uint32_t>(in);
    if (a.version_ != 5 || a.platform_ != 1) fail("Only retail PC PAK v5/platform 1 is supported");
    auto archive_size = fs::file_size(path);
    if (count > archive_size / 17) fail("Impossible archive entry count");
    a.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry e;
        for (;;) {
            int c = in.get();
            if (c == EOF) fail("Unterminated path in archive index");
            if (!c) break;
            if (e.path_bytes.size() >= 32767) fail("Archive path is too long");
            e.path_bytes.push_back(static_cast<std::uint8_t>(c));
        }
        e.path_utf8 = cp1252_to_utf8(e.path_bytes);
        e.logical_offset = read_le<std::uint32_t>(in);
        e.original_size = read_le<std::uint32_t>(in);
        e.filetime_ticks = read_le<std::uint64_t>(in);
        a.entries_.push_back(std::move(e));
    }
    auto pos = in.tellg();
    if (pos < 0) fail("Invalid archive index");
    a.data_base_ = static_cast<std::uint64_t>(pos);
    for (std::size_t i = 0; i < a.entries_.size(); ++i) {
        auto& e = a.entries_[i];
        if (e.logical_offset < 13) fail("Invalid retail PC entry offset");
        e.physical_offset = a.data_base_ + e.logical_offset - 13;
        auto next = i + 1 < a.entries_.size()
                        ? a.data_base_ + a.entries_[i + 1].logical_offset - 13
                        : archive_size;
        if (e.physical_offset < a.data_base_ || next < e.physical_offset || next > archive_size)
            fail("Archive entry offset is out of bounds");
        e.stored_size = next - e.physical_offset;
        if (a.type_ == ArchiveType::stored && e.stored_size != e.original_size)
            fail("Stored entry size does not match its index length");
    }
    return a;
}

VerifyReport Archive::verify(bool full, Progress progress) const {
    VerifyReport report;
    std::ifstream in(path_, std::ios::binary);
    if (!in) fail("Cannot reopen archive");
    std::uint64_t done = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        auto const& e = entries_[i];
        (void)safe_relative(e.path_utf8);
        report.original_bytes += e.original_size;
        report.stored_bytes += e.stored_size;
        if (full) stream_entry(type_, in, e, nullptr, true, report.compressed_blocks);
        done += e.original_size;
        if (progress && !progress(i + 1, entries_.size(), e.path_utf8)) fail("Operation canceled");
    }
    return report;
}

void Archive::extract(const fs::path& destination, std::span<const std::size_t> selection,
                      bool overwrite, Progress progress) const {
    fs::create_directories(destination);
    std::vector<std::size_t> indices;
    if (selection.empty()) { indices.resize(entries_.size()); for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i; }
    else indices.assign(selection.begin(), selection.end());
    std::ifstream in(path_, std::ios::binary);
    if (!in) fail("Cannot reopen archive");
    std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> collisions;
    std::uint64_t blocks = 0;
    for (std::size_t k = 0; k < indices.size(); ++k) {
        if (indices[k] >= entries_.size()) fail("Invalid selected entry index");
        auto const& e = entries_[indices[k]];
        auto rel = safe_relative(e.path_utf8);
        auto key = lower_ascii(path_to_utf8(rel));
        auto prior = collisions.find(key);
        if (prior != collisions.end()) {
            auto crc = stream_entry(type_, in, e, nullptr, false, blocks);
            if (prior->second.first != e.original_size || prior->second.second != crc)
                fail("Different archive entries collide on Windows: " + e.path_utf8);
        } else {
            auto target = destination / rel;
            fs::create_directories(target.parent_path());
            if (fs::exists(target) && !overwrite) fail("Output file already exists: " + path_to_utf8(target));
            auto temp = target; temp += L".pakman.tmp";
            std::error_code ec; fs::remove(temp, ec);
            std::uint32_t crc{};
            try {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) fail("Cannot create output file: " + path_to_utf8(temp));
                crc = stream_entry(type_, in, e, &out, false, blocks);
                out.close();
                if (!out) fail("Failed closing output file: " + path_to_utf8(temp));
                if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    fail("Cannot commit extracted file: " + win_error(GetLastError()));
            } catch (...) { fs::remove(temp, ec); throw; }
            set_filetime(target, e.filetime_ticks);
            collisions.emplace(key, std::pair{e.original_size, crc});
        }
        if (progress && !progress(k + 1, indices.size(), e.path_utf8)) fail("Operation canceled");
    }
}

void create_archive(const fs::path& source, const fs::path& destination,
                    const CreateOptions& options, Progress progress) {
    if (!fs::is_directory(source)) fail("Source is not a directory");
    if (fs::exists(destination)) fail("Destination already exists");
    struct Source { fs::path disk; std::string name; std::vector<std::uint8_t> bytes; std::uint32_t size; std::uint64_t time; std::streampos patch; };
    std::vector<Source> files;
    for (auto const& item : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) {
        if (item.is_symlink()) fail("Symbolic links are not supported: " + path_to_utf8(item.path()));
        if (!item.is_regular_file()) continue;
        auto size = item.file_size();
        if (size > std::numeric_limits<std::uint32_t>::max()) fail("A source file exceeds the PAK 32-bit limit");
        auto u8 = item.path().lexically_relative(source).generic_u8string();
        std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
        (void)safe_relative(name);
        files.push_back({item.path(), name, utf8_to_cp1252(name), static_cast<std::uint32_t>(size), filetime_for(item.path()), {}});
    }
    if (options.deterministic) std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) { return a.name < b.name; });
    auto temp = destination; temp += L".tmp";
    std::error_code ec; fs::remove(temp, ec);
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) fail("Cannot create temporary archive");
        out.write(options.type == ArchiveType::stored ? "PAKA" : "PAKC", 4);
        write_le<std::uint32_t>(out, 5); write_le<std::uint32_t>(out, 1);
        if (files.size() > std::numeric_limits<std::uint32_t>::max()) fail("Too many source files");
        write_le<std::uint32_t>(out, static_cast<std::uint32_t>(files.size()));
        for (auto& f : files) {
            out.write(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()); out.put('\0');
            f.patch = out.tellp(); write_le<std::uint32_t>(out, 0); write_le<std::uint32_t>(out, f.size); write_le<std::uint64_t>(out, f.time);
        }
        auto data_base = static_cast<std::uint64_t>(out.tellp());
        std::array<unsigned char, 4096> buffer{};
        for (std::size_t i = 0; i < files.size(); ++i) {
            auto physical = static_cast<std::uint64_t>(out.tellp());
            auto logical64 = physical - data_base + 13;
            if (logical64 > std::numeric_limits<std::uint32_t>::max()) fail("Archive offsets exceed the PAK 32-bit limit");
            auto resume = out.tellp(); out.seekp(files[i].patch); write_le<std::uint32_t>(out, static_cast<std::uint32_t>(logical64)); out.seekp(resume);
            std::ifstream input(files[i].disk, std::ios::binary);
            if (!input) fail("Cannot open source file: " + path_to_utf8(files[i].disk));
            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                auto n = input.gcount(); if (!n) break;
                if (options.type == ArchiveType::stored) out.write(reinterpret_cast<char*>(buffer.data()), n);
                else {
                    auto packed = deflate_block(std::span(buffer.data(), static_cast<std::size_t>(n)));
                    write_le<std::uint32_t>(out, static_cast<std::uint32_t>(packed.size()));
                    write_le<std::uint32_t>(out, static_cast<std::uint32_t>(n));
                    out.write(reinterpret_cast<const char*>(packed.data()), packed.size());
                }
            }
            if (options.type == ArchiveType::compressed) write_le<std::uint32_t>(out, 0);
            if (!out) fail("Failed writing archive data");
            if (progress && !progress(i + 1, files.size(), files[i].name)) fail("Operation canceled");
        }
        out.close();
        auto check = Archive::open(temp); check.verify(true);
        fs::rename(temp, destination);
    } catch (...) { fs::remove(temp, ec); throw; }
}

std::string type_name(ArchiveType type) { return type == ArchiveType::stored ? "PAKA (stored)" : "PAKC (compressed)"; }

} // namespace pakman
