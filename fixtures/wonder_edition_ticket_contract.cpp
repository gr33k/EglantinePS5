#ifdef WONDER_FRONTEND_TICKET
#include "switch2_ticket.hpp"
#else
#include "emuhub_switch2_ticket.hpp"
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path scratch = argv[1];
    fs::create_directories(scratch);
    using namespace emuhub_switch2;
    assert(source_matches(kKey, kSource));
    assert(!source_matches(kEditionKey, kEditionSource));
    assert(edition_source_matches(kEditionKey, kEditionSource));
    assert(!edition_source_matches(kKey, kSource));
    assert(kEditionRootSha == "460c025c23fcbd32fffb49ce87647bf0a7d895f21af5ecbd1c6717dbf227efdc");
    assert(kEditionManifest == "462f96f78686b4083dce956d69cff7360733308b048abb0b9617bf7dcadf25bd");
    assert(kEditionBaseId == 0x010015100B514000ULL && kEditionUpdateVersion == 327680);
    assert(kEditionRomFS.find("/Wonder-010015100B514000-edition-327680/romfs") != std::string_view::npos);
    const auto base = ticket(0x1234);
    const auto parsed_base = parse(base);
    assert(parsed_base && !parsed_base->edition && parsed_base->boot == kBoot);
    const auto edition = edition_ticket(0x5678);
    const auto parsed_edition = parse(edition);
    assert(parsed_edition && parsed_edition->edition && parsed_edition->boot == kEditionBoot);
    auto bad_root = edition;
    bad_root[bad_root.find(std::string(kEditionRootSha))] = 'f';
    assert(!parse(bad_root));
    auto bad_boot = edition;
    bad_boot[bad_boot.find(std::string(kEditionBoot))] = 'x';
    assert(!parse(bad_boot));
    assert(!parse("EMUHUB-SWITCH2-EDITION-1\n0000000000000000\n"));
    const auto path = scratch / "request.txt";
    { std::ofstream out(path); out << edition; }
    const auto consumed = consume(path.c_str());
    assert(consumed && consumed->edition && consumed->key_hash == 0x5678);
    assert(!fs::exists(path));
    assert(!consume(path.c_str()));
    const auto receipt = scratch / "edition-ready.txt";
    { std::ofstream out(receipt);
      out << "EMUHUB-SWITCH2-EDITION-READY-1\n" << kEditionRootSha << "\n"
          << kEditionManifest << "\n"; }
    assert(edition_receipt_at(receipt.string()));
    assert(!edition_receipt_ready());
    assert(!edition_ready());
    std::cout << "PASS legacy base ticket unchanged; Edition receipt accepted before USB access; exact ticket/tamper/missing receipt/one-use\n";
}
