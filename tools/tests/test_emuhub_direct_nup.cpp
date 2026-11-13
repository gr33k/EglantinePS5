#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "../../headless/emuhub_direct_nup.hpp"
#include "../../headless/forwarded_launch.h"
#include "../../headless/ps5_switch_safe_menu.hpp"

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("emuhub-nup-check-" + std::to_string(getpid()));
    fs::create_directories(root);
    const auto game = root / "Game [0400ABCD12340000].nup";
    { std::ofstream out(game); out << "NUPTEST"; }
    const std::string path = game.string();
    const std::string base = root.string();
    assert(emuhub_direct_nup::under(path, base));
    assert(emuhub_direct_nup::admit_at(path, base));
    assert(!emuhub_direct_nup::under(base + "/nested/Game.nup", base));
    assert(!emuhub_direct_nup::under(base + "/../Game.nup", base));
    assert(!emuhub_direct_nup::under(base + "/Game.nsp", base));
    assert(!emuhub_direct_nup::under(base + "/Game.nup/other", base));
    assert(!emuhub_direct_nup::under(base + "/Game.NUP", base));
    const auto link = root / "link.nup";
    fs::create_symlink(game, link);
    assert(!emuhub_direct_nup::admit_at(link.string(), base));
    const auto missing = root / "missing.nup";
    assert(!emuhub_direct_nup::admit_at(missing.string(), base));
    char arg0[]="eboot.bin", arg1[]="--rom", arg3[]="--exit-after-game";
    auto arg2=path;
    char* argv[]={arg0,arg1,arg2.data(),arg3,nullptr};
    const auto forwarded=Eden::ParseForwardedArgs(4,argv);
    assert(forwarded.rom==path && forwarded.exit_after_game);
    assert(Eden::ResolveForwardedRom(forwarded.rom,"/unused")==path);
    emuhub_switch_menu::State menu;
    using namespace emuhub_switch_menu;
    assert(menu.consume(touchpad|options)==Action::opened);
    assert(menu.captures_game_input());
    assert(menu.consume(0)==Action::none);
    assert(menu.consume(down)==Action::none);
    assert(menu.consume(0)==Action::none);
    assert(menu.consume(cross)==Action::none); // warning page
    assert(menu.page()==Page::confirm_return);
    assert(menu.consume(0)==Action::none);
    assert(menu.consume(circle)==Action::none); // cancel Return
    assert(menu.page()==Page::choices);
    menu.consume(0); menu.consume(down); menu.consume(0);
    menu.consume(cross); menu.consume(0);
    assert(menu.consume(cross)==Action::exit_confirmed);
    fs::remove_all(root);
}
