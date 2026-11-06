// SPDX-License-Identifier: GPL-3.0-or-later
// Two exact private base rows. NUPs, updates and wildcard USB paths are excluded.
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
inline constexpr std::string_view kReadyBody = "EMUHUB-SWITCH2-READY-1\n57d67e432525412a14e641bdeb4a719ca63b08e7ef8bb2f810eed28f89ea7df1\n19016\n3759363036\n";
inline constexpr std::string_view kYoshiKey = "Yoshi-0400245021D2C000.dir";
inline constexpr std::string_view kYoshiSource = "ns2:0400245021D2C000:base";
inline constexpr std::string_view kYoshiBoot = "/mnt/usb0/eglantineps5/roms/Yoshi-0400245021D2C000/exefs/main";
inline constexpr std::string_view kYoshiReady = "/app0/config/switch2-yoshi-verified.txt";
inline constexpr std::string_view kYoshiManifest = "165bd33e804583957617936ea3c694bea368a1c5241274a012ad1eb5c2d0a76b";
inline constexpr std::string_view kUsbMarker = "/mnt/usb0/homebrew/EmuHub/PPSA91337/storage-id.txt";
inline constexpr std::string_view kRequest = "/app0/config/switch2-request.txt";
inline constexpr std::string_view kReturn = "/app0/config/switch2-return.txt";
inline constexpr std::string_view kMagic = "EMUHUB-SWITCH2-1\n";
enum class Route { wonder, yoshi };
inline std::uint64_t key_hash(std::string_view key) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : key) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}
inline std::optional<Route> select(std::string_view key, std::string_view source) {
    if (key == kKey && source == kSource) return Route::wonder;
    if (key == kYoshiKey && source == kYoshiSource) return Route::yoshi;
    return std::nullopt;
}
inline bool source_matches(std::string_view key, std::string_view source) {
    return select(key, source).has_value();
}
inline std::string_view boot(Route route) {
    return route == Route::wonder ? kBoot : kYoshiBoot;
}
inline std::string_view manifest(Route route) {
    return route == Route::wonder ? kManifest : kYoshiManifest;
}
inline std::string_view key(Route route) {
    return route == Route::wonder ? kKey : kYoshiKey;
}
inline std::string ticket(std::uint64_t hash, Route route) {
    if (!hash || hash != key_hash(key(route))) return {};
    char digits[17]{};
    std::snprintf(digits, sizeof digits, "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(kMagic) + digits + "\n" + std::string(manifest(route)) +
           "\n" + std::string(boot(route)) + "\n";
}
struct Ticket { std::uint64_t key_hash; std::string boot; Route route; };
inline std::optional<Ticket> parse(std::string_view body) {
    if (!body.starts_with(kMagic)) return std::nullopt;
    body.remove_prefix(kMagic.size());
    if (body.size() < 17 || body[16] != '\n') return std::nullopt;
    std::uint64_t hash = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const char c = body[i];
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
        if (digit == 16) return std::nullopt;
        hash = (hash << 4) | digit;
    }
    for (Route route : {Route::wonder, Route::yoshi}) {
        if (hash == key_hash(key(route)) &&
            body.substr(17) == std::string(manifest(route)) + "\n" +
                               std::string(boot(route)) + "\n")
            return Ticket{hash, std::string(boot(route)), route};
    }
    return std::nullopt;
}
inline bool regular_file(std::string_view path, std::uint64_t bytes) {
    const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info{};
    const bool okay = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size >= 0 &&
        static_cast<std::uint64_t>(info.st_size) == bytes;
    ::close(fd);
    return okay;
}
inline std::optional<std::string> read_small(std::string_view path,
                                              std::size_t maximum) {
    const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    struct stat info{};
    const bool valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 &&
        static_cast<std::size_t>(info.st_size) <= maximum;
    std::string data(valid ? static_cast<std::size_t>(info.st_size) : 0, '\0');
    std::size_t done = 0;
    while (valid && done < data.size()) {
        const ssize_t count = ::read(fd, data.data() + done, data.size() - done);
        if (count <= 0) break;
        done += static_cast<std::size_t>(count);
    }
    const bool closed = ::close(fd) == 0;
    if (!valid || !closed || done != data.size()) return std::nullopt;
    return data;
}
inline std::optional<std::string> parse_marker(std::string_view raw) {
    constexpr std::string_view prefix = "emuhub-ps5-storage-v1\nPPSA91337\n";
    if (!raw.starts_with(prefix) ||
        raw.size() != prefix.size() + 33 || raw.back() != '\n')
        return std::nullopt;
    const std::string id(raw.substr(prefix.size(), 32));
    for (const char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::nullopt;
    return id;
}
inline std::optional<std::string> usb_marker_id() {
    const auto raw = read_small(kUsbMarker, 95);
    return raw ? parse_marker(*raw) : std::nullopt;
}
inline bool yoshi_receipt_matches(std::string_view raw, std::string_view id) {
    if (id.size() != 32) return false;
    const std::string expected = "EMUHUB-SWITCH2-USB-1\n" +
        std::string(kYoshiManifest) + "\n154\n20982621365\n" +
        std::string(id) + "\n";
    return raw == expected;
}
inline bool ready_at(std::string_view game_boot, std::string_view receipt) {
    if (game_boot == kBoot)
        return regular_file(game_boot, 28034256) &&
               read_small(receipt, 160) == std::optional<std::string>(std::string(kReadyBody));
    if (game_boot != kYoshiBoot || !regular_file(game_boot, 75050700))
        return false;
    const auto id = usb_marker_id();
    if (!id) return false;
    const auto raw = read_small(receipt, 160);
    return raw && yoshi_receipt_matches(*raw, *id);
}
inline bool receipt_ready(Route route) {
    return route == Route::wonder ?
        read_small(kReady, 160) == std::optional<std::string>(std::string(kReadyBody)) :
        ready_at(kYoshiBoot, kYoshiReady);
}
inline bool ready(Route route) {
    return route == Route::wonder ? ready_at(kBoot, kReady) :
                                   ready_at(kYoshiBoot, kYoshiReady);
}
inline bool ready(std::string_view selected_boot) {
    if (selected_boot == kBoot) return ready(Route::wonder);
    if (selected_boot == kYoshiBoot) return ready(Route::yoshi);
    return false;
}
inline std::optional<Ticket> consume(const char* path) {
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    struct stat info{};
    char bytes[256]{};
    const bool bounded = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && info.st_size < static_cast<off_t>(sizeof bytes);
    const ssize_t count = bounded ? ::read(fd, bytes, sizeof bytes) : -1;
    const bool closed = ::close(fd) == 0;
    if (::unlink(path) != 0 || !closed || !bounded || count != info.st_size)
        return std::nullopt;
    return parse(std::string_view(bytes, static_cast<std::size_t>(count)));
}
} // namespace emuhub_switch2
