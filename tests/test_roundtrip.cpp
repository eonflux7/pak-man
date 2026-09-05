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

template<class T> static void write_le(std::ostream& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out.put(static_cast<char>(value >> (i * 8)));
}

static void write_ps2_prototype_fixture(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out.write("PAKA", 4);
    write_le<std::uint32_t>(out, 3);
    write_le<std::uint32_t>(out, 2);
    write_le<std::uint32_t>(out, 2);
    out.write("Maps/ST04.txt", 13); out.put('\0');
    write_le<std::uint32_t>(out, 0);
    write_le<std::uint32_t>(out, 9);
    write_le<std::uint64_t>(out, 0);
    out.write("Maps/data.bin", 13); out.put('\0');
    write_le<std::uint32_t>(out, 9);
    write_le<std::uint32_t>(out, 4);
    write_le<std::uint64_t>(out, 0);
    out.write("prototype", 9);
    out.write("\x00\x01\x02\x03", 4);
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
                assert(archive.version() == (platform == pakman::Platform::pc ? 5u : 4u));
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

        auto replacement = root / "replacement.pak";
        pakman::create_archive(root / "input", replacement);
        auto original_archive = read(replacement);
        { std::ofstream(root / "input" / "hello.txt", std::ios::binary) << "replacement\n"; }
        bool refused_overwrite = false;
        try { pakman::create_archive(root / "input", replacement); }
        catch (std::exception const&) { refused_overwrite = true; }
        assert(refused_overwrite);
        assert(read(replacement) == original_archive);

        pakman::create_archive(root / "input", replacement, {.overwrite = true});
        auto replaced_archive = pakman::Archive::open(replacement);
        replaced_archive.verify(true);
        auto replacement_output = root / "replacement-out";
        replaced_archive.extract(replacement_output);
        assert(read(replacement_output / "hello.txt") == "replacement\n");

        auto prototype = root / "ps2-prototype.pak";
        write_ps2_prototype_fixture(prototype);
        auto prototype_archive = pakman::Archive::open(prototype);
        assert(prototype_archive.type() == pakman::ArchiveType::stored);
        assert(prototype_archive.version() == 3);
        assert(prototype_archive.platform() == static_cast<std::uint32_t>(pakman::Platform::ps2));
        assert(prototype_archive.entries().size() == 2);
        auto prototype_report = prototype_archive.verify(true);
        assert(prototype_report.original_bytes == 13);
        auto prototype_output = root / "prototype-out";
        prototype_archive.extract(prototype_output);
        assert(read(prototype_output / "Maps" / "ST04.txt") == "prototype");
        assert(read(prototype_output / "Maps" / "data.bin") == std::string("\x00\x01\x02\x03", 4));

        fs::remove_all(root);
        std::cout << "round-trip tests passed\n";
        return 0;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
