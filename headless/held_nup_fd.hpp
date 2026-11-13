// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace emuhub_direct_nup {

struct ExpectedSource {
    std::string path;
    std::uint8_t volume_id{};
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t size{};
    std::array<std::uint8_t, 32> sha256{};
    std::array<std::uint8_t, 32> toc_sha256{};
};

inline constexpr std::string_view RootForVolume(std::uint8_t id) {
    switch (id) {
    case 1: return "/mnt/ext1/eglantineps5/roms";
    case 2: return "/mnt/usb0/eglantineps5/roms";
    case 3: return "/data/eglantineps5/roms";
    default: return {};
    }
}

inline std::uint64_t ReadLe64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(data[i]) << (i * 8);
    return value;
}

class HeldNupFd final {
public:
    HeldNupFd(const HeldNupFd&) = delete;
    HeldNupFd& operator=(const HeldNupFd&) = delete;
    ~HeldNupFd() { if (fd_ >= 0) ::close(fd_); }

    static std::shared_ptr<HeldNupFd> OpenAt(std::string_view path, std::string_view root,
                                             std::uint64_t max_size, std::string* error) {
        const auto fail = [&](const char* why) -> std::shared_ptr<HeldNupFd> {
            if (error) *error = why;
            return {};
        };
        if (root.empty() || path.size() <= root.size() + 1 ||
            path.substr(0, root.size()) != root || path[root.size()] != '/') return fail("root");
        const auto name = path.substr(root.size() + 1);
        if (name.size() < 5 || name.size() > 240 || !name.ends_with(".nup") ||
            name.find("..") != name.npos || name.find('/') != name.npos ||
            name.find('\\') != name.npos) return fail("name");
        for (unsigned char c : name) if (c < 0x20 || c == 0x7f) return fail("name");
        const int dir = ::open(std::string(root).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (dir < 0) return fail("open root");
        const int fd = ::openat(dir, std::string(name).c_str(), O_RDONLY | O_NOFOLLOW);
        ::close(dir);
        if (fd < 0) return fail("open source");
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            static_cast<std::uint64_t>(st.st_size) > max_size) {
            ::close(fd);
            return fail("source shape");
        }
        return std::shared_ptr<HeldNupFd>(new HeldNupFd(fd, std::string(path), st));
    }

    // Bound the untrusted TOC before the core allocates it. This checks only
    // NUP structure, not every game byte; member hashes are checked on read.
    bool ValidateContainer(std::string* error) const {
        const auto fail = [&](const char* why) {
            if (error) *error = why;
            return false;
        };
        std::array<std::uint8_t, 0x80> super{};
        if (Read(super.data(), super.size(), 0) != super.size() ||
            std::memcmp(super.data(), "NUP\x01", 4) != 0 ||
            (std::uint16_t(super[4]) | (std::uint16_t(super[5]) << 8)) < 0x80 ||
            ReadLe64(super.data() + 0x40) != Size()) return fail("NUP superblock");
        const auto toc_offset = ReadLe64(super.data() + 0x08);
        const auto toc_size = ReadLe64(super.data() + 0x10);
        const auto blob_offset = ReadLe64(super.data() + 0x38);
        if (toc_offset < super.size() || toc_size < 0x30 ||
            toc_size > 64ull * 1024 * 1024 ||
            blob_offset > Size() || toc_offset > blob_offset ||
            toc_size > blob_offset - toc_offset ||
            blob_offset % 0x10 != 0) return fail("NUP TOC bounds");
        std::array<std::uint8_t, 32> expected{};
        std::memcpy(expected.data(), super.data() + 0x18, expected.size());
        std::array<std::uint8_t, 32> actual{};
        if (!DigestRange(toc_offset, toc_size, actual) || actual != expected)
            return fail("NUP TOC digest");
        struct stat after{};
        if (::fstat(fd_, &after) != 0 || after.st_dev != initial_.st_dev ||
            after.st_ino != initial_.st_ino || after.st_size != initial_.st_size ||
            after.st_mtime != initial_.st_mtime || after.st_ctime != initial_.st_ctime)
            return fail("source changed");
        return true;
    }

    bool Verify(const ExpectedSource& expected, std::string* error) const {
        return VerifyForRoot(expected, RootForVolume(expected.volume_id), error);
    }

    // The root argument also lets the host fixture use a private temporary directory.
    bool VerifyForRoot(const ExpectedSource& expected, std::string_view root,
                       std::string* error) const {
        const auto fail = [&](const char* why) {
            if (error) *error = why;
            return false;
        };
        if (path_ != expected.path || root.empty() || path_.size() <= root.size() + 1 ||
            path_.substr(0, root.size()) != root || path_[root.size()] != '/' ||
            !SameIdentity(initial_, expected))
            return fail("source identity");
        std::array<std::uint8_t, 0x80> super{};
        if (Read(super.data(), super.size(), 0) != super.size() ||
            std::memcmp(super.data(), "NUP\x01", 4) != 0 ||
            (std::uint16_t(super[4]) | (std::uint16_t(super[5]) << 8)) < 0x80 ||
            ReadLe64(super.data() + 0x40) != expected.size) return fail("NUP superblock");
        const auto toc_offset = ReadLe64(super.data() + 0x08);
        const auto toc_size = ReadLe64(super.data() + 0x10);
        const auto blob_offset = ReadLe64(super.data() + 0x38);
        if (toc_offset < super.size() || toc_size < 0x30 || toc_size > 64 * 1024 * 1024 ||
            blob_offset > expected.size || toc_offset > blob_offset ||
            toc_size > blob_offset - toc_offset) return fail("NUP TOC bounds");
        if (!EqualDigest(super.data() + 0x18, expected.toc_sha256)) return fail("TOC quote");
        std::array<std::uint8_t, 32> actual_toc{};
        if (!DigestRange(toc_offset, toc_size, actual_toc) || actual_toc != expected.toc_sha256)
            return fail("TOC digest");
        std::array<std::uint8_t, 32> actual_full{};
        if (!DigestRange(0, expected.size, actual_full) || actual_full != expected.sha256)
            return fail("source digest");
        struct stat after{};
        if (::fstat(fd_, &after) != 0 || !SameIdentity(after, expected) ||
            after.st_mtime != initial_.st_mtime || after.st_ctime != initial_.st_ctime)
            return fail("source changed during verify");
        return true;
    }

    std::size_t Read(void* out, std::size_t length, std::uint64_t offset) const {
        if (offset >= static_cast<std::uint64_t>(initial_.st_size)) return 0;
        length = std::min(length, static_cast<std::size_t>(initial_.st_size - offset));
        for (;;) {
            const auto got = ::pread(fd_, out, length, static_cast<off_t>(offset));
            if (got < 0 && errno == EINTR) continue;
            return got < 0 ? 0 : static_cast<std::size_t>(got);
        }
    }

    std::size_t Size() const { return static_cast<std::size_t>(initial_.st_size); }
    const std::string& Path() const { return path_; }

private:
    HeldNupFd(int fd, std::string path, const struct stat& st)
        : fd_(fd), path_(std::move(path)), initial_(st) {}

    static bool SameIdentity(const struct stat& st, const ExpectedSource& expected) {
        return std::uint64_t(st.st_dev) == expected.device &&
               std::uint64_t(st.st_ino) == expected.inode &&
               std::uint64_t(st.st_size) == expected.size;
    }
    static bool EqualDigest(const std::uint8_t* bytes, const std::array<std::uint8_t, 32>& digest) {
        return std::memcmp(bytes, digest.data(), digest.size()) == 0;
    }
    bool DigestRange(std::uint64_t offset, std::uint64_t size,
                     std::array<std::uint8_t, 32>& digest) const {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) return false;
        bool okay = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
        std::array<std::uint8_t, 1024 * 1024> buffer{};
        while (okay && size) {
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
            const auto got = Read(buffer.data(), amount, offset);
            if (got != amount || EVP_DigestUpdate(context, buffer.data(), got) != 1) {
                okay = false;
                break;
            }
            offset += got;
            size -= got;
        }
        unsigned length = 0;
        okay = okay && EVP_DigestFinal_ex(context, digest.data(), &length) == 1 &&
               length == digest.size();
        EVP_MD_CTX_free(context);
        return okay;
    }

    int fd_{};
    std::string path_;
    struct stat initial_{};
};

} // namespace emuhub_direct_nup
