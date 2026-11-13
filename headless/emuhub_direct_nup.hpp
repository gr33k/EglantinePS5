// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Direct root NUP launch from a signed EmuHub provider invocation. This
// validates a path, not a transfer receipt; the wheel owns source publication.
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace emuhub_direct_nup {
inline constexpr std::string_view kRoots[] = {
    "/mnt/ext1/eglantineps5/roms",
    "/mnt/usb0/eglantineps5/roms",
    "/data/eglantineps5/roms",
};
inline bool name_ok(std::string_view name) {
    if (name.size() < 5 || name.size() > 240 || name == "." || name == ".." ||
        !name.ends_with(".nup") || name.find("..") != name.npos) return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
    return true;
}
inline bool under(std::string_view file, std::string_view root) {
    return file.size() > root.size() + 1 && file.substr(0, root.size()) == root &&
           file[root.size()] == '/' && name_ok(file.substr(root.size() + 1));
}
inline bool admit_at(std::string_view file, std::string_view root) {
    if (!under(file, root)) return false;
    const int dir = ::open(std::string(root).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return false;
    const std::string name(file.substr(root.size() + 1));
    const int fd = ::openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat statbuf{};
    const bool okay = fd >= 0 && ::fstat(fd, &statbuf) == 0 &&
        S_ISREG(statbuf.st_mode) && statbuf.st_size > 0 &&
        static_cast<std::uint64_t>(statbuf.st_size) <= 64ull * 1024 * 1024 * 1024;
    if (fd >= 0) ::close(fd);
    ::close(dir);
    return okay;
}
inline bool path_ok(std::string_view file) {
    for (const auto root : kRoots) if (under(file, root)) return true;
    return false;
}
inline bool admit(std::string_view file) {
    for (const auto root : kRoots) if (admit_at(file, root)) return true;
    return false;
}
} // namespace emuhub_direct_nup
