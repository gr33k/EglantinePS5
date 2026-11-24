// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// Isolated provider-side ABI parser for the proposed EH2NUP1 record. No
// production key or nonce service is embedded here; absent either, deny.
namespace emuhub_nup_proof {
using Bytes16 = std::array<std::uint8_t, 16>;
using Bytes20 = std::array<std::uint8_t, 20>;
using Bytes32 = std::array<std::uint8_t, 32>;
struct Source {
    std::string path;
    std::uint8_t volume_id = 0;
    std::uint64_t device = 0, inode = 0, size = 0;
    Bytes32 sha256{}, toc_sha256{};
};
struct Record {
    Bytes16 nonce{};
    Bytes32 nup_source_revision{};
    Source source;
};
using ConsumeNonce = bool (*)(const Bytes16 &, void *);
using VerifySignature = bool (*)(std::span<const std::uint8_t>,
                                 std::span<const std::uint8_t, 64>,
                                 const Bytes32 &, void *);

inline std::uint64_t le(std::span<const std::uint8_t> s) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < s.size(); ++i) v |= std::uint64_t(s[i]) << (8 * i);
    return v;
}
template <std::size_t N>
inline std::array<std::uint8_t, N> field(std::span<const std::uint8_t> s,
                                         std::size_t offset) {
    std::array<std::uint8_t, N> out{};
    std::memcpy(out.data(), s.data() + offset, N);
    return out;
}
inline std::string_view root(std::uint8_t id) {
    switch (id) {
    case 1: return "/mnt/ext1/eglantineps5/roms/";
    case 2: return "/mnt/usb0/eglantineps5/roms/";
    case 3: return "/data/eglantineps5/roms/";
    default: return {};
    }
}
template <std::size_t N>
inline bool nonzero(const std::array<std::uint8_t, N> &value) {
    for (auto c : value) if (c) return true;
    return false;
}
inline bool utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        if (c < 0x80) { ++i; continue; }
        unsigned extra = 0;
        std::uint32_t cp = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 0x1f; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 0x0f; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (i + extra >= s.size()) return false;
        for (unsigned j = 1; j <= extra; ++j) {
            const auto b = static_cast<std::uint8_t>(s[i + j]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += extra + 1;
    }
    return true;
}
inline bool path_ok(std::string_view path, std::uint8_t volume_id) {
    const auto prefix = root(volume_id);
    if (prefix.empty() || !path.starts_with(prefix)) return false;
    const auto leaf = path.substr(prefix.size());
    if (leaf.size() < 5 || leaf.size() > 240 || !leaf.ends_with(".nup") ||
        leaf.find("..") != leaf.npos || !utf8(leaf)) return false;
    for (unsigned char c : leaf)
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
    return true;
}
inline bool parse(std::span<const std::uint8_t> bytes, std::uint64_t now_unix_ms,
                  const Bytes16 &expected_key_id, const Bytes32 &public_key,
                  const Bytes20 &expected_provider_revision,
                  const Bytes32 &expected_provider, const Bytes32 &expected_pair,
                  VerifySignature verify, void *verify_context,
                  ConsumeNonce consume, void *context, Record &out) {
    out = {};
    if (!verify || !consume || bytes.size() < 337 || bytes.size() > 1024 ||
        std::memcmp(bytes.data(), "EH2NUP1\0", 8) != 0 ||
        le(bytes.subspan(8, 2)) != 1) return false;
    const auto path_len = le(bytes.subspan(265, 2));
    if (path_len == 0 || path_len > 688 ||
        le(bytes.subspan(10, 2)) != bytes.size() ||
        bytes.size() != 336 + path_len ||
        field<16>(bytes, 12) != expected_key_id) return false;
    for (std::size_t i = 267; i < 272; ++i) if (bytes[i]) return false;
    const auto issued = le(bytes.subspan(44, 8));
    const auto expiry = le(bytes.subspan(52, 8));
    if (!issued || issued > now_unix_ms || expiry < now_unix_ms ||
        expiry < issued || expiry - issued > 300000) return false;
    if (field<20>(bytes, 60) != expected_provider_revision ||
        !nonzero(field<32>(bytes, 80)) ||
        !nonzero(field<16>(bytes, 28)) ||
        !nonzero(field<32>(bytes, 176)) ||
        !nonzero(field<32>(bytes, 208)) ||
        field<32>(bytes, 112) != expected_provider ||
        field<32>(bytes, 144) != expected_pair) return false;
    const auto volume = bytes[264];
    const std::string path(reinterpret_cast<const char *>(bytes.data() + 272),
                           static_cast<std::size_t>(path_len));
    if (!path_ok(path, volume)) return false;
    const auto signed_size = bytes.size() - 64;
    const std::span<const std::uint8_t, 64> signature(bytes.data() + signed_size, 64);
    if (!verify(bytes.first(signed_size), signature, public_key,
                verify_context)) return false;
    Record candidate;
    candidate.nonce = field<16>(bytes, 28);
    candidate.nup_source_revision = field<32>(bytes, 80);
    candidate.source.path = path;
    candidate.source.volume_id = volume;
    candidate.source.device = le(bytes.subspan(240, 8));
    candidate.source.inode = le(bytes.subspan(248, 8));
    candidate.source.size = le(bytes.subspan(256, 8));
    candidate.source.sha256 = field<32>(bytes, 176);
    candidate.source.toc_sha256 = field<32>(bytes, 208);
    if (!candidate.source.device || !candidate.source.inode ||
        !candidate.source.size || candidate.source.size > 64ull * 1024 * 1024 * 1024 ||
        !consume(candidate.nonce, context)) return false;
    out = std::move(candidate);
    return true;
}
} // namespace emuhub_nup_proof
