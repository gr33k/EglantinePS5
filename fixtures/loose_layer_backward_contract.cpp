#include "extracted_game.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
static void write(const fs::path& p) { fs::create_directories(p.parent_path()); std::ofstream(p) << "x"; }
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path root = argv[1];
    const auto yoshi = root / "Yoshi-0400245021D2C000";
    write(yoshi / "exefs/main");
    write(yoshi / "updates/131072/exefs/main");
    assert(Eden::ExtractedGameMain(yoshi.string()) == (yoshi / "exefs/main").string());
    const auto unrelated = root / "Other-0100000000000000";
    write(unrelated / "base/exefs/main");
    write(unrelated / "updates/327680/exefs/main");
    assert(Eden::ExtractedGameMain(unrelated.string()) == (unrelated / "base/exefs/main").string());
    const auto wonder = root / "Wonder-010015100B514000";
    write(wonder / "exefs/main");
    write(wonder / "updates/327680/exefs/main");
    assert(Eden::ExtractedGameMain(wonder.string()) == (wonder / "exefs/main").string());
    const auto edition = root / "Wonder-010015100B514000-edition-327680";
    write(edition / "exefs/main");
    assert(Eden::ExtractedGameMain(edition.string()) == (edition / "exefs/main").string());
    std::cout << "PASS Yoshi, unrelated, Wonder base unchanged; distinct Edition root selected\n";
}
