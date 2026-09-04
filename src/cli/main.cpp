#include "pakman/archive.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void usage() {
    std::cout <<
        "pakman-cli - Commandos: Strike Force PAK tool\n\n"
        "  pakman-cli list <archive.pak>\n"
        "  pakman-cli verify <archive.pak> [--quick]\n"
        "  pakman-cli extract <archive.pak> -o <directory> [--overwrite]\n"
        "  pakman-cli create <directory> -o <archive.pak> [--type stored|compressed]\n";
}

static fs::path option_value(int argc, char** argv, std::string_view option) {
    for (int i = 0; i + 1 < argc; ++i) if (argv[i] == option) return fs::u8path(argv[i + 1]);
    return {};
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return argc == 1 ? 0 : 1; }
        std::string command = argv[1];
        if (command == "list") {
            auto archive = pakman::Archive::open(fs::u8path(argv[2]));
            std::cout << pakman::type_name(archive.type()) << ", version " << archive.version()
                      << ", platform " << archive.platform() << ", " << archive.entries().size() << " entries\n";
            std::cout << "      Original       Stored  Path\n";
            for (auto const& e : archive.entries())
                std::cout << std::setw(14) << e.original_size << std::setw(13) << e.stored_size << "  " << e.path_utf8 << '\n';
        } else if (command == "verify") {
            auto archive = pakman::Archive::open(fs::u8path(argv[2]));
            bool full = true;
            for (int i = 3; i < argc; ++i) if (std::string_view(argv[i]) == "--quick") full = false;
            auto report = archive.verify(full, [](auto done, auto total, auto) {
                if (done == total || done % 100 == 0) std::cerr << "\rVerified " << done << '/' << total;
                return true;
            });
            std::cerr << '\n';
            std::cout << "OK: " << archive.entries().size() << " entries, " << report.original_bytes
                      << " original bytes, " << report.stored_bytes << " stored bytes, "
                      << report.compressed_blocks << " compressed blocks\n";
        } else if (command == "extract") {
            auto output = option_value(argc, argv, "-o");
            if (output.empty()) { usage(); return 1; }
            bool overwrite = false;
            for (int i = 3; i < argc; ++i) if (std::string_view(argv[i]) == "--overwrite") overwrite = true;
            auto archive = pakman::Archive::open(fs::u8path(argv[2]));
            archive.extract(output, {}, overwrite, [](auto done, auto total, auto current) {
                std::cerr << "\rExtracted " << done << '/' << total << "  " << current.substr(0, 60) << "          ";
                return true;
            });
            std::cerr << "\nExtraction complete\n";
        } else if (command == "create") {
            auto output = option_value(argc, argv, "-o");
            if (output.empty()) { usage(); return 1; }
            pakman::CreateOptions options;
            auto type = option_value(argc, argv, "--type").string();
            if (type == "compressed") options.type = pakman::ArchiveType::compressed;
            else if (!type.empty() && type != "stored") { usage(); return 1; }
            pakman::create_archive(fs::u8path(argv[2]), output, options, [](auto done, auto total, auto current) {
                std::cerr << "\rPacked " << done << '/' << total << "  " << current.substr(0, 60) << "          ";
                return true;
            });
            std::cerr << "\nCreated " << output.string() << '\n';
        } else { usage(); return 1; }
        return 0;
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 2;
    }
}

