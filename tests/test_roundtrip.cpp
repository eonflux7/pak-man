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
    { std::ofstream(root / "input" / u8"cañonazo.txt", std::ios::binary); }
    try {
        for (auto type : {pakman::ArchiveType::stored, pakman::ArchiveType::compressed}) {
            auto pak = root / (type == pakman::ArchiveType::stored ? "test-a.pak" : "test-c.pak");
            pakman::create_archive(root / "input", pak, {.type = type});
            auto archive = pakman::Archive::open(pak);
            assert(archive.type() == type);
            assert(archive.entries().size() == 3);
            auto report = archive.verify(true);
            assert(report.original_bytes == 15010);
            auto output = root / (type == pakman::ArchiveType::stored ? "out-a" : "out-c");
            archive.extract(output);
            assert(read(output / "hello.txt") == read(root / "input" / "hello.txt"));
            assert(read(output / "Gfx" / "blocks.bin") == read(root / "input" / "Gfx" / "blocks.bin"));
            assert(fs::exists(output / u8"cañonazo.txt"));
        }
        fs::remove_all(root);
        std::cout << "round-trip tests passed\n";
        return 0;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
