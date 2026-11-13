#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <openssl/sha.h>
#include "held_nup_fd.hpp"

namespace fs = std::filesystem;
using namespace emuhub_direct_nup;

static std::array<std::uint8_t, 32> Hash(const std::uint8_t* bytes, std::size_t size) {
    std::array<std::uint8_t, 32> out{};
    SHA256(bytes, size, out.data());
    return out;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    assert(input);
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
    assert(bytes.size() > 0x80);
    const auto toc_offset = ReadLe64(bytes.data() + 0x08);
    const auto toc_size = ReadLe64(bytes.data() + 0x10);
    assert(toc_offset + toc_size <= bytes.size());
    const auto dir = fs::temp_directory_path() / ("held-nup-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const auto original = dir / "original.nup";
    const auto moved = dir / "moved.nup";
    { std::ofstream out(original, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    struct stat st{};
    assert(::stat(original.c_str(), &st) == 0);
    ExpectedSource quote;
    quote.path = original.string();
    quote.device = std::uint64_t(st.st_dev);
    quote.inode = std::uint64_t(st.st_ino);
    quote.size = std::uint64_t(st.st_size);
    quote.sha256 = Hash(bytes.data(), bytes.size());
    quote.toc_sha256 = Hash(bytes.data() + toc_offset, toc_size);
    std::string error;
    auto held = HeldNupFd::OpenAt(quote.path, dir.string(), 64ull << 30, &error);
    assert(held && error.empty());
    assert(held->ValidateContainer(&error));
    assert(held->VerifyForRoot(quote, dir.string(), &error));
    fs::rename(original, moved);
    { std::ofstream out(original, std::ios::binary); out << "SWAPPED"; }
    std::array<char, 4> magic{};
    assert(held->Read(magic.data(), magic.size(), 0) == magic.size());
    assert(std::memcmp(magic.data(), "NUP\x01", 4) == 0);
    assert(held->ValidateContainer(&error));
    assert(held->VerifyForRoot(quote, dir.string(), &error));
    auto wrong = quote;
    wrong.sha256[0] ^= 1;
    assert(!held->VerifyForRoot(wrong, dir.string(), &error));
    { std::fstream out(moved, std::ios::binary | std::ios::in | std::ios::out); out.seekp(0); out.put('X'); }
    assert(!held->VerifyForRoot(quote, dir.string(), &error));
    { std::fstream out(moved, std::ios::binary | std::ios::in | std::ios::out);
      const std::array<char, 8> oversized_toc{1, 0, 0, 4, 0, 0, 0, 0};
      out.seekp(0); out.put(78); out.seekp(0x10); out.write(oversized_toc.data(), oversized_toc.size()); }
    assert(!held->ValidateContainer(&error) && error == "NUP TOC bounds");
    held.reset();
    fs::remove_all(dir);
    std::puts("held-nup-fd: PASS");
}
