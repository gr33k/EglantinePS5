#include "emuhub_switch2_ticket.hpp"
#include "extracted_game.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
static void write(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path scratch = argv[1];
    fs::create_directories(scratch);
    using namespace emuhub_switch2;
    assert(ocarina_source_matches(kOcarinaKey, kOcarinaSource));
    assert(!ocarina_source_matches(kEditionKey, kEditionSource));
    assert(!ocarina_source_matches(kKey, kSource));
    assert(kOcarinaBaseId == 0x0400486024DEC000ULL);
    assert(kOcarinaUpdatePackageId == (kOcarinaBaseId | 0x800ULL));
    assert(kOcarinaUpdateVersion == 131072);
    assert(kOcarinaManifest == "ec4c99a77c8dbe81ea773b0e12ec894cb0d69137dc26643b3ad1d9c6f8615344");
    assert(kOcarinaMainSha == "7db9ddfda088c22d219ef13496decd7077c68cd67d4b37f53f4ac46a082768f2");
    assert(kOcarinaBoot.find("/Ocarina-0400486024DEC000-update-131072/exefs/main") != std::string_view::npos);
    const auto base = ticket(0x1234);
    const auto parsed_base = parse(base);
    assert(parsed_base && !parsed_base->edition && !parsed_base->ocarina_update);
    const auto wonder = edition_ticket(0x5678);
    const auto parsed_wonder = parse(wonder);
    assert(parsed_wonder && parsed_wonder->edition && !parsed_wonder->ocarina_update);
    const auto installed_base = scratch / "Ocarina-0400486024DEC000";
    write(installed_base / "exefs/main");
    write(installed_base / "updates/131072/exefs/main");
    assert(Eden::ExtractedGameMain(installed_base.string()) == (installed_base / "exefs/main").string());
    const auto versioned = scratch / "Ocarina-0400486024DEC000-update-131072";
    write(versioned / "exefs/main");
    assert(Eden::ExtractedGameMain(versioned.string()) == (versioned / "exefs/main").string());
    const auto receipt = scratch / "ocarina-ready.txt";
    if (!fs::exists(receipt)) {
        std::ofstream out(receipt);
        out << "EMUHUB-SWITCH2-OCARINA-READY-1\n" << kOcarinaRootSha << "\n" << kOcarinaTocSha << "\n"
            << kOcarinaManifest << "\n" << kOcarinaMainSha << "\n"
            << kOcarinaMergedFiles << "\n" << kOcarinaMergedBytes << "\n";
    }
    if (!ocarina_manifest_pinned()) {
        assert(ocarina_ticket(0x9abc).empty());
        assert(!ocarina_receipt_at(receipt.string()));
        assert(!parse(std::string(kOcarinaMagic) + "0000000000009abc\n"));
        std::cout << "PASS pending root hash hard-gates Ocarina; base/Wonder and distinct update folder preserved\n";
        return 0;
    }
    assert(ocarina_receipt_at(receipt.string()));
    const auto request = ocarina_ticket(0x9abc);
    const auto parsed = parse(request);
    assert(parsed && parsed->ocarina_update && !parsed->edition && parsed->boot == kOcarinaBoot);
    auto bad = request;
    bad[bad.find(std::string(kOcarinaManifest))] = 102;
    assert(!parse(bad));
    auto bad_root = request;
    bad_root[bad_root.find(std::string(kOcarinaRootSha))] = 98;
    assert(!parse(bad_root));
    { std::ofstream out(receipt, std::ios::trunc); out << "bad"; }
    assert(!ocarina_receipt_at(receipt.string()));
    const auto request_file = scratch / "request.txt";
    { std::ofstream out(request_file); out << request; }
    const auto consumed = consume(request_file.c_str());
    assert(consumed && consumed->ocarina_update && consumed->key_hash == 0x9abc);
    assert(!consume(request_file.c_str()));
    assert(!ocarina_ready()); // No PS5 USB tree or /app0 receipt in host fixture.
    std::cout << "PASS exact Ocarina ticket/receipt; tamper/one-use; base/Wonder and versioned update route\n";
}
