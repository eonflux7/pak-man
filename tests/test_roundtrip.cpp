#include "pakman/archive.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int main() {
    auto root = fs::temp_directory_path() / "pakman-roundtrip-test";
    std::error_code ec; fs::remove_all(root, ec);
    fs::create_directories(root / "input" / "Gfx");
    { std::ofstream(root / "input" / "hello.txt", std::ios::binary) << "hello pak\n"; }
    { std::ofstream out(root / "input" / "Gfx" / "blocks.bin", std::ios::binary); for (int i = 0; i < 15000; ++i) out.put(static_cast<char>(i * 31)); }
    { std::ofstream(root / "input" / u8"cañonazoFU.sp", std::ios::binary) << "unicode path\n"; }
    try {
        for (auto platform : {pakman::Platform::pc, pakman::Platform::ps2, pakman::Platform::xbox}) {
            for (auto type : {pakman::ArchiveType::stored, pakman::ArchiveType::compressed}) {
                auto stem = pakman::platform_name(platform) + (type == pakman::ArchiveType::stored ? "-a.pak" : "-c.pak");
                auto pak = root / stem;
                pakman::create_archive(root / "input", pak, {.type = type, .platform = platform});
                auto archive = pakman::Archive::open(pak);
                assert(archive.type() == type);
                assert(archive.version() == 5);
                assert(archive.platform() == static_cast<std::uint32_t>(platform));
                assert(archive.entries().size() == 3);
                auto report = archive.verify(true);
                assert(report.original_bytes == 15023);
                auto output = root / (pakman::platform_name(platform) + (type == pakman::ArchiveType::stored ? "-out-a" : "-out-c"));
                archive.extract(output);
                assert(read(output / "hello.txt") == read(root / "input" / "hello.txt"));
                assert(read(output / "Gfx" / "blocks.bin") == read(root / "input" / "Gfx" / "blocks.bin"));
                assert(read(output / u8"cañonazoFU.sp") == read(root / "input" / u8"cañonazoFU.sp"));
            }
        }
        fs::remove_all(root);
        std::cout << "round-trip tests passed\n";
        return 0;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
