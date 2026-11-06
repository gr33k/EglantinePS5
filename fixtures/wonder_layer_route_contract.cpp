#include "extracted_game.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
int main(int argc,char**argv){
 assert(argc==2);fs::path root=argv[1];
 fs::create_directories(root/"base/exefs");fs::create_directories(root/"updates/327680/exefs");
 std::ofstream(root/"base/exefs/main")<<"base";
 std::ofstream(root/"updates/327680/exefs/main")<<"update";
 assert(Eden::ExtractedGameMain(root.string())==(root/"base/exefs/main").string());
 fs::remove(root/"base/exefs/main");
 fs::create_directories(root/"exefs");std::ofstream(root/"exefs/main")<<"installed-base";
 assert(Eden::ExtractedGameMain(root.string())==(root/"exefs/main").string());
 fs::remove(root/"exefs/main");
 assert(Eden::ExtractedGameMain(root.string())==(root/"updates/327680/exefs/main").string());
 fs::create_directories(root/"updates/65536/exefs");std::ofstream(root/"updates/65536/exefs/main")<<"older";
 assert(Eden::ExtractedGameMain(root.string())==(root/"updates/327680/exefs/main").string());
 fs::remove(root/"updates/327680/exefs/main");fs::remove(root/"updates/65536/exefs/main");
 assert(Eden::ExtractedGameMain(root.string()).empty());
 std::cout<<"PASS nested and installed-root base win; update selected only without base; numeric newest; absent refuses\n";
}
