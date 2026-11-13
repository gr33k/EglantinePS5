// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "held_nup_fd.hpp"
#include "core/file_sys/vfs/vfs_real.h"

namespace emuhub_direct_nup {

class HeldVfsFile final : public FileSys::VfsFile {
public:
    explicit HeldVfsFile(std::shared_ptr<HeldNupFd> source) : source_(std::move(source)) {}
    std::string GetName() const override {
        const auto& path = source_->Path();
        return path.substr(path.find_last_of('/') + 1);
    }
    std::size_t GetSize() const override { return source_->Size(); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return {}; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        return source_->Read(data, length, offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    std::shared_ptr<HeldNupFd> source_;
};

class HeldNupFilesystem final : public FileSys::RealVfsFilesystem {
public:
    explicit HeldNupFilesystem(std::shared_ptr<HeldNupFd> source)
        : path_(source->Path()), file_(std::make_shared<HeldVfsFile>(std::move(source))) {}

    FileSys::VirtualFile BoundFile() const { return file_; }
    FileSys::VfsEntryType GetEntryType(std::string_view path) const override {
        if (path == path_) return FileSys::VfsEntryType::File;
        return RealVfsFilesystem::GetEntryType(path);
    }
    FileSys::VirtualFile OpenFile(std::string_view path, FileSys::OpenMode perms) override {
        if (path == path_) return perms == FileSys::OpenMode::Read ? file_ : nullptr;
        return RealVfsFilesystem::OpenFile(path, perms);
    }

private:
    std::string path_;
    FileSys::VirtualFile file_;
};

} // namespace emuhub_direct_nup
