// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace Eden {

struct ForwardedArgs {
    std::string rom;
    bool exit_after_game = false;
};

inline ForwardedArgs ParseForwardedArgs(int argc, char** argv) {
    ForwardedArgs result;
    if (argc <= 0 || argv == nullptr) return result;
    constexpr std::string_view rom_flag = "--rom";
    constexpr std::string_view rom_prefix = "--rom=";
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr) break;
        const std::string_view arg{argv[i]};
        if (arg == "--exit-after-game") {
            result.exit_after_game = true;
        } else if (arg == rom_flag) {
            if (i + 1 < argc && argv[i + 1] != nullptr) result.rom = argv[++i];
        } else if (arg.starts_with(rom_prefix)) {
            result.rom = std::string{arg.substr(rom_prefix.size())};
        }
    }
    return result;
}

struct ProofArgs {
    bool requested=false, valid=false;
    std::string path;
};
inline ProofArgs ParseProofArgs(int argc,char** argv) {
    ProofArgs out;
    if(argc<=0||!argv)return out;
    for(int i=1;i<argc;++i)
        if(argv[i]&&std::string_view(argv[i])=="--emuhub-proof")
            out.requested=true;
    if(!out.requested)return out;
    if(argc!=4||!argv[1]||!argv[2]||!argv[3])return out;
    constexpr std::string_view root="/data/emuhub/broker/nup-requests/";
    bool exit=false;
    for(int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        if(arg=="--exit-after-game") {
            if(exit)return out;
            exit=true;
        } else if(arg=="--emuhub-proof") {
            if(i+1>=argc||!argv[++i])return out;
            out.path=argv[i];
        } else return out;
    }
    if(!exit||!std::string_view(out.path).starts_with(root)||
       out.path.size()!=root.size()+32+5||
       !std::string_view(out.path).ends_with(".ehnp"))return out;
    for(char c:std::string_view(out.path).substr(root.size(),32))
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return out;
    out.valid=true;return out;
}

inline std::string ResolveForwardedRom(std::string_view rom, std::string_view roms_dir) {
    while (!rom.empty() && (rom.back() == ' ' || rom.back() == '\r' || rom.back() == '\n')) rom.remove_suffix(1);
    if (rom.empty()) return {};
    if (rom.front() == '/') return std::string{rom};
    for (std::string_view rest = rom; !rest.empty();) {
        const auto slash = rest.find('/');
        if (rest.substr(0, slash) == "..") return {};
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    std::string path{roms_dir};
    if (!path.empty() && path.back() != '/') path += '/';
    path += rom;
    return path;
}

}
