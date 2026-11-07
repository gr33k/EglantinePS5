#pragma once
#include <openssl/evp.h>
#include <cstdint>
#include <string>
#include <string_view>
namespace emuhub_signature {
inline constexpr unsigned char kPublicKey[32] = {0xdf,0xe1,0x74,0x03,0x7b,0x1b,0xc6,0xd4,0x74,0xdf,0x42,0x7d,0x91,0x7a,0x45,0x45,0xea,0x26,0xa0,0x3f,0xf2,0x6a,0x31,0x35,0x4a,0xbe,0x32,0x21,0x3e,0x29,0xa6,0x86};
inline int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
inline bool verify(std::string_view body,std::string_view hexsig){
 if(hexsig.size()!=128)return false;
 unsigned char raw[64];for(unsigned i=0;i<64;++i){int hi=nibble(hexsig[2*i]),lo=nibble(hexsig[2*i+1]);if(hi<0||lo<0)return false;raw[i]=static_cast<unsigned char>((hi<<4)|lo);}
 EVP_PKEY* key=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,kPublicKey,32);
 EVP_MD_CTX* ctx=EVP_MD_CTX_new();
 bool ok=key&&ctx&&EVP_DigestVerifyInit(ctx,nullptr,nullptr,nullptr,key)==1&&EVP_DigestVerify(ctx,raw,64,reinterpret_cast<const unsigned char*>(body.data()),body.size())==1;
 EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);return ok;
}
}
