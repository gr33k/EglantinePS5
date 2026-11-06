// SPDX-License-Identifier: GPL-3.0-or-later
// One prepared Switch 2 beta row. Root NUP packages never enter this route.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace emuhub_switch2 {
inline constexpr std::string_view kKey = "Wonder-010015100B514000.dir";
inline constexpr std::string_view kSource = "ns2:010015100B514000:base";
inline constexpr std::string_view kBoot = "/mnt/ext1/eglantineps5/roms/Wonder-010015100B514000/exefs/main";
inline constexpr std::string_view kReady = "/app0/config/switch2-wonder-verified.txt";
inline constexpr std::string_view kManifest = "57d67e432525412a14e641bdeb4a719ca63b08e7ef8bb2f810eed28f89ea7df1";
inline constexpr std::string_view kRequest = "/app0/config/switch2-request.txt";
inline constexpr std::string_view kReturn = "/app0/config/switch2-return.txt";
inline constexpr std::string_view kMagic = "EMUHUB-SWITCH2-1\n";
inline constexpr std::string_view kReadyBody = "EMUHUB-SWITCH2-READY-1\n57d67e432525412a14e641bdeb4a719ca63b08e7ef8bb2f810eed28f89ea7df1\n19016\n3759363036\n";
// Edition ticket binds the TOC-derived merged and AOC manifests. Activation
// still requires full source extraction and destination readback before receipt install.
inline constexpr std::string_view kEditionKey = "Wonder-010015100B514000-edition-1.2.0.dir";
inline constexpr std::string_view kEditionSource = "ns2:010015100B514000:edition:327680:aoc:010015100B515001";
inline constexpr std::string_view kEditionBoot = "/mnt/usb0/eglantineps5/roms/Wonder-010015100B514000-edition-327680/exefs/main";
inline constexpr std::string_view kEditionAoc = "/mnt/usb0/eglantineps5/roms/Wonder-010015100B514000-edition-327680/aoc/010015100B515001/romfs";
inline constexpr std::string_view kEditionReady = "/app0/config/switch2-wonder-edition-verified.txt";
inline constexpr std::string_view kEditionRootSha = "460c025c23fcbd32fffb49ce87647bf0a7d895f21af5ecbd1c6717dbf227efdc";
inline constexpr std::string_view kEditionManifest = "462f96f78686b4083dce956d69cff7360733308b048abb0b9617bf7dcadf25bd";
inline constexpr std::string_view kEditionMagic = "EMUHUB-SWITCH2-EDITION-1\n";
inline constexpr std::uint64_t kEditionAocId = 0x010015100B515001ULL;

inline bool source_matches(std::string_view key, std::string_view source) {
    return key == kKey && source == kSource;
}
inline bool edition_source_matches(std::string_view key, std::string_view source) {
    return key == kEditionKey && source == kEditionSource;
}
inline std::string ticket(std::uint64_t hash) {
    if (!hash) return {};
    char digits[17]{};
    std::snprintf(digits, sizeof digits, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(kMagic) + digits + "\n" + std::string(kManifest) + "\n" + std::string(kBoot) + "\n";
}
struct Ticket { std::uint64_t key_hash; std::string boot; bool edition = false; };
inline bool edition_manifest_pinned() {
    return kEditionManifest.size() == 64 && kEditionManifest.find_first_not_of('0') != std::string_view::npos;
}
inline std::string edition_ticket(std::uint64_t hash) {
    if (!hash || !edition_manifest_pinned()) return {};
    char digits[17]{};
    std::snprintf(digits, sizeof digits, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(kEditionMagic) + digits + "\n" + std::string(kEditionRootSha) + "\n" +
           std::string(kEditionManifest) + "\n" + std::string(kEditionBoot) + "\n";
}
inline std::optional<Ticket> parse(std::string_view body) {
    const bool edition = body.starts_with(kEditionMagic);
    if (!edition && !body.starts_with(kMagic)) return std::nullopt;
    if (edition && !edition_manifest_pinned()) return std::nullopt;
    body.remove_prefix(edition ? kEditionMagic.size() : kMagic.size());
    if (body.size() < 17 || body[16] != '\n') return std::nullopt;
    std::uint64_t hash = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const char c = body[i];
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
        if (digit == 16) return std::nullopt;
        hash = (hash << 4) | digit;
    }
    const std::string expected = edition ?
        std::string(kEditionRootSha) + "\n" + std::string(kEditionManifest) + "\n" + std::string(kEditionBoot) + "\n" :
        std::string(kManifest) + "\n" + std::string(kBoot) + "\n";
    if (!hash || body.substr(17) != expected) return std::nullopt;
    return Ticket{hash, std::string(edition ? kEditionBoot : kBoot), edition};
}
inline bool regular_file(std::string_view path, std::uint64_t bytes) {
    const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info{};
    const bool okay = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        static_cast<std::uint64_t>(info.st_size) == bytes;
    ::close(fd);
    return okay;
}
inline bool ready_at(std::string_view boot, std::string_view receipt) {
    if (!regular_file(boot, 28034256)) return false;
    const int fd = ::open(std::string(receipt).c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info{};
    char bytes[160]{};
    const bool bounded = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size == static_cast<off_t>(kReadyBody.size());
    const ssize_t count = bounded ? ::read(fd, bytes, sizeof bytes) : -1;
    const bool closed = ::close(fd) == 0;
    return bounded && closed && count == info.st_size &&
        std::string_view(bytes, static_cast<std::size_t>(count)) == kReadyBody;
}
inline bool receipt_ready() {
    const int fd = ::open(kReady.data(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info{};
    char bytes[160]{};
    const bool bounded = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size == static_cast<off_t>(kReadyBody.size());
    const ssize_t count = bounded ? ::read(fd, bytes, sizeof bytes) : -1;
    const bool closed = ::close(fd) == 0;
    return bounded && closed && count == info.st_size &&
        std::string_view(bytes, static_cast<std::size_t>(count)) == kReadyBody;
}
inline bool ready() { return ready_at(kBoot, kReady); }
inline bool edition_ready() {
    if (!edition_manifest_pinned() || !regular_file(kEditionBoot, 29043641)) return false;
    const int dir = ::open(kEditionAoc.data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return false;
    struct stat info{};
    const bool directory = ::fstat(dir, &info) == 0 && S_ISDIR(info.st_mode);
    ::close(dir);
    if (!directory) return false;
    const std::string expected = "EMUHUB-SWITCH2-EDITION-READY-1\n" +
        std::string(kEditionRootSha) + "\n" + std::string(kEditionManifest) + "\n";
    const int fd = ::open(kEditionReady.data(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    char bytes[256]{};
    const bool bounded = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                         info.st_size == static_cast<off_t>(expected.size());
    const ssize_t count = bounded ? ::read(fd, bytes, sizeof bytes) : -1;
    const bool closed = ::close(fd) == 0;
    return bounded && closed && count == info.st_size &&
           std::string_view(bytes, static_cast<std::size_t>(count)) == expected;
}
inline std::optional<Ticket> consume(const char* path) {
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    struct stat info{};
    char bytes[512]{};
    const bool bounded = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && info.st_size < static_cast<off_t>(sizeof bytes);
    const ssize_t count = bounded ? ::read(fd, bytes, sizeof bytes) : -1;
    const bool closed = ::close(fd) == 0;
    if (::unlink(path) != 0 || !closed || !bounded || count != info.st_size)
        return std::nullopt;
    return parse(std::string_view(bytes, static_cast<std::size_t>(count)));
}
} // namespace emuhub_switch2
