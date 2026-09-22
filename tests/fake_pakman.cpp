#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "verify") {
        std::ifstream archive(argv[2], std::ios::binary);
        std::string magic;
        std::getline(archive, magic);
        return archive && magic == "fake-pakman-archive" ? 0 : 2;
    }
    if (argc < 5 || std::string_view(argv[1]) != "create") return 1;
    fs::path output;
    for (int i = 3; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "-o") output = argv[i + 1];
    if (output.empty() || !fs::is_directory(argv[2])) return 2;
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(argv[2]))
        if (entry.is_regular_file()) files.push_back(fs::relative(entry.path(), argv[2]));
    std::ranges::sort(files, {}, [](const fs::path& path) { return path.generic_string(); });
    std::ofstream archive(output, std::ios::binary);
    archive << "fake-pakman-archive\n";
    for (const auto& path : files) {
        archive << path.generic_string() << '\n';
        std::ifstream input(fs::path(argv[2]) / path, std::ios::binary);
        archive << input.rdbuf();
    }
    return archive ? 0 : 2;
}
