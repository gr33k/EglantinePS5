// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "emuhub_nup_proof.hpp"
#include "held_nup_fd.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace emuhub_proof_admission {
namespace proof=emuhub_nup_proof;
inline constexpr std::string_view kRequestRoot="/data/emuhub/broker/nup-requests";
// The linked Helper must authenticate a durable, fsynced transfer receipt for
// this exact held inode/source revision/digest and prove exclusive writer
// ownership. No receipt, stale source, or ambiguous owner must deny. The
// callback must not compute a new whole-file digest at Play time.
using VerifyFinalReceipt = bool (*)(const proof::Record&,
                                    const emuhub_direct_nup::HeldNupFd&, void*);
struct Authority {
    proof::Bytes16 key_id{};
    proof::Bytes32 public_key{},provider_sha256{},pair_sha256{};
    proof::Bytes20 provider_revision{};
    std::uint64_t now_unix_ms=0;
    uid_t broker_uid=static_cast<uid_t>(-1);
    proof::ConsumeNonce consume_nonce=nullptr;
    void *nonce_context=nullptr;
    VerifyFinalReceipt verify_final_receipt=nullptr;
    void *receipt_context=nullptr;
};
inline bool verify_ed25519(std::span<const std::uint8_t> message,
                           std::span<const std::uint8_t,64> signature,
                           const proof::Bytes32 &key,void *) {
    EVP_PKEY *p=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,
                                             key.data(),key.size());
    if(!p)return false;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    const bool okay=ctx &&
        EVP_DigestVerifyInit(ctx,nullptr,nullptr,nullptr,p)==1 &&
        EVP_DigestVerify(ctx,signature.data(),signature.size(),
                         message.data(),message.size())==1;
    EVP_MD_CTX_free(ctx);EVP_PKEY_free(p);return okay;
}
inline std::string hex_nonce(const proof::Bytes16 &nonce) {
    constexpr char hex[]="0123456789abcdef";
    std::string out(32,'0');
    for(std::size_t i=0;i<nonce.size();++i) {
        out[2*i]=hex[nonce[i]>>4];out[2*i+1]=hex[nonce[i]&15];
    }
    return out;
}
inline bool request_name(std::string_view path,std::string_view root) {
    if(!path.starts_with(root)||path.size()!=root.size()+1+32+5||
       path[root.size()]!='/')return false;
    const auto leaf=path.substr(root.size()+1);
    if(!leaf.ends_with(".ehnp"))return false;
    for(char c:leaf.substr(0,32))
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
inline int open_request_root(std::string_view root) {
    if(root!=kRequestRoot)
        return open(std::string(root).c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    int fd=open("/data",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return -1;
    for(const char* part:{"emuhub","broker","nup-requests"}) {
        const int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        close(fd);
        if(next<0)return -1;
        fd=next;
    }
    return fd;
}
inline bool read_record(std::string_view path,std::string_view root,
                        uid_t broker_uid,std::vector<std::uint8_t> &bytes) {
    bytes.clear();
    if(!request_name(path,root))return false;
    const int dir=open_request_root(root);
    if(dir<0)return false;
    struct stat directory{};
    if(fstat(dir,&directory)!=0||!S_ISDIR(directory.st_mode)||
       directory.st_uid!=broker_uid||
       (root==kRequestRoot && (directory.st_mode&0777)!=0700)) {
        close(dir);return false;
    }
    const std::string leaf(path.substr(root.size()+1));
    const int fd=openat(dir,leaf.c_str(),O_RDONLY|O_NOFOLLOW);
    close(dir);
    if(fd<0)return false;
    struct stat before{},after{};
    bool okay=fstat(fd,&before)==0 && S_ISREG(before.st_mode) &&
        before.st_nlink==1 && before.st_uid==broker_uid &&
        (before.st_mode&0777)==0600 &&
        before.st_size>=337 && before.st_size<=1024;
    if(okay) {
        bytes.resize(static_cast<std::size_t>(before.st_size));
        std::size_t at=0;
        while(at<bytes.size()) {
            const auto got=pread(fd,bytes.data()+at,bytes.size()-at,
                                 static_cast<off_t>(at));
            if(got<0&&errno==EINTR)continue;
            if(got<=0){okay=false;break;}
            at+=static_cast<std::size_t>(got);
        }
        okay=okay && fstat(fd,&after)==0 &&
            before.st_dev==after.st_dev && before.st_ino==after.st_ino &&
            before.st_size==after.st_size &&
            before.st_mtime==after.st_mtime && before.st_ctime==after.st_ctime;
    }
    close(fd);
    if(!okay)bytes.clear();
    return okay;
}
inline std::shared_ptr<emuhub_direct_nup::HeldNupFd> admit(
        std::string_view path,std::string_view request_root,
        const Authority *authority,std::string *error) {
    const auto fail=[&](const char* why) -> std::shared_ptr<emuhub_direct_nup::HeldNupFd> {
        if(error)*error=why;
        return {};
    };
    if(!authority||!authority->consume_nonce||!authority->now_unix_ms||
       authority->broker_uid==static_cast<uid_t>(-1))
        return fail("proof authority unavailable");
    if(!authority->verify_final_receipt)
        return fail("verified transfer receipt unavailable; retransfer");
    std::vector<std::uint8_t> bytes;
    if(!read_record(path,request_root,authority->broker_uid,bytes))
        return fail("proof file");
    proof::Bytes16 nonce{};
    std::memcpy(nonce.data(),bytes.data()+28,nonce.size());
    if(path.substr(request_root.size()+1,32)!=hex_nonce(nonce))
        return fail("proof filename nonce");
    proof::Record record;
    if(!proof::parse(bytes,authority->now_unix_ms,authority->key_id,
                     authority->public_key,authority->provider_revision,
                     authority->provider_sha256,authority->pair_sha256,
                     verify_ed25519,nullptr,authority->consume_nonce,
                     authority->nonce_context,record))
        return fail("proof signature, expiry or nonce");
    const auto root=emuhub_direct_nup::RootForVolume(record.source.volume_id);
    if(root.empty())return fail("proof volume");
    auto held=emuhub_direct_nup::HeldNupFd::OpenAt(
        record.source.path,root,64ull*1024*1024*1024,error);
    if(!held)return {};
    emuhub_direct_nup::ExpectedSource expected{
        record.source.path,record.source.volume_id,record.source.device,
        record.source.inode,record.source.size,record.source.sha256,
        record.source.toc_sha256};
    if(!held->VerifyReceiptIdentityForRoot(expected,root,error))
        return fail("final identity changed; retransfer");
    if(!held->ValidateContainer(error))
        return fail("NUP index invalid; reprepare");
    if(!authority->verify_final_receipt(record,*held,authority->receipt_context))
        return fail("verified transfer receipt stale; retransfer");
    if(!held->VerifyReceiptIdentityForRoot(expected,root,error))
        return fail("final changed during admission; retransfer");
    return held;
}
} // namespace emuhub_proof_admission
