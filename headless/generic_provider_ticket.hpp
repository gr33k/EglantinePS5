// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "signature_verify.hpp"
#include "layout_verify.hpp"
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
namespace emuhub_generic {
inline constexpr const char* kTicket="/data/homebrew/PPSA91337/config/ns2-prepare/launch.txt";
inline constexpr const char* kUsb="/mnt/usb0";
struct Route {std::string boot,romfs;std::vector<std::pair<std::uint64_t,std::string>> aoc;std::uint64_t program=0;std::uint32_t version=0;};
inline bool hex(std::string_view s,size_t n){if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;return true;}
inline bool token(std::string_view s){if(s.empty()||s.size()>96||s=="."||s=="..")return false;for(char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;return true;}
inline bool decimal(std::string_view s,std::uint64_t& out){if(s.empty()||s.size()>20)return false;out=0;for(char c:s){if(c<'0'||c>'9'||out>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10)return false;out=out*10+static_cast<unsigned>(c-'0');}return true;}
inline std::string read_small(const char* path,size_t max){int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return {};struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&static_cast<size_t>(st.st_size)<=max;std::string s;if(ok){s.resize(static_cast<size_t>(st.st_size));size_t at=0;while(at<s.size()){ssize_t n=::read(fd,s.data()+at,s.size()-at);if(n<=0){ok=false;break;}at+=static_cast<size_t>(n);}}close(fd);return ok?s:std::string{};}
inline std::vector<std::string> lines(const std::string& s){std::vector<std::string> out;if(s.empty()||s.back()!='\n')return out;size_t p=0;while(p<s.size()){size_t q=s.find('\n',p);if(q==std::string::npos||q-p>512)return {};out.push_back(s.substr(p,q-p));p=q+1;}return out;}
inline bool signed_fields(const std::string&text,std::vector<std::string>&fields){fields=lines(text);if(fields.size()<2||fields.back().size()!=128)return false;size_t cut=text.rfind('\n',text.size()-2);return cut!=std::string::npos&&emuhub_signature::verify(std::string_view(text).substr(0,cut+1),fields.back());}
inline bool directory(const std::string&path){int fd=emuhub_layout::open_dir_path(path);if(fd<0)return false;close(fd);return true;}
inline bool regular(const std::string&path){int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW);if(fd<0)return false;struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0;close(fd);return ok;}
inline std::optional<Route> consume_at(const char* ticket_path,const char* usb_root){
 std::string text=read_small(ticket_path,4096);if(text.empty())return std::nullopt;
 if(unlink(ticket_path)!=0)return std::nullopt;
 std::vector<std::string> f;if(!signed_fields(text,f)||f.size()<12||f[0]!="EMUHUB-NS2-LAUNCH-2"||!hex(f[1],64)||!hex(f[2],64)||!hex(f[3],64)||!hex(f[4],64)||!hex(f[5],64)||!hex(f[6],32)||!token(f[7])||!hex(f[8],16))return std::nullopt;
 std::uint64_t program=0,version=0,count=0;if(!decimal(f[9],version)||version>UINT32_MAX||!decimal(f[10],count)||count>8||f.size()!=12+count)return std::nullopt;
 for(char c:f[8])program=(program<<4)|static_cast<unsigned>(c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c-'A'+10);
 if(!program)return std::nullopt;
 const std::string usb=usb_root,root=usb+"/eglantineps5/roms/"+f[7];
 if(read_small((usb+"/homebrew/EmuHub/PPSA91337/storage-id.txt").c_str(),128)!="emuhub-ps5-storage-v1\nPPSA91337\n"+f[6]+"\n")return std::nullopt;
 auto receipt=lines(read_small((root+"/.verified").c_str(),1024));if(receipt.size()!=10||receipt[0]!="EMUHUB-NUP-VERIFIED-1"||receipt[1]!=f[1]||receipt[2]!=f[2]||receipt[3]!=f[3]||receipt[4]!=f[4]||receipt[5]!=f[5]||receipt[6]!=f[6]||receipt[7]!=f[7])return std::nullopt;
 std::uint64_t files=0,bytes=0;if(!decimal(receipt[8],files)||!decimal(receipt[9],bytes)||!files||!bytes)return std::nullopt;
 const std::string manifest=usb+"/eglantineps5/requests/"+f[1]+"/"+f[2]+"/selected.txt";
 if(!emuhub_layout::verify(root,manifest,f[5],f[3],f[4],files,bytes))return std::nullopt;
 Route route;route.boot=root+"/exefs/main";route.romfs=root+"/romfs";route.program=program;route.version=static_cast<std::uint32_t>(version);
 if(!regular(route.boot)||!directory(route.romfs))return std::nullopt;
 std::uint64_t previous=0;for(size_t i=0;i<count;++i){if(!hex(f[11+i],16))return std::nullopt;std::uint64_t id=0;for(char c:f[11+i])id=(id<<4)|static_cast<unsigned>(c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c-'A'+10);if(id<=previous||id==program||(id&~0x1fffULL)!=(program&~0x1fffULL))return std::nullopt;previous=id;auto aoc=root+"/aoc/"+f[11+i]+"/romfs";if(!directory(aoc))return std::nullopt;route.aoc.push_back({id,aoc});}
 return route;
}
inline std::optional<Route> consume(){return consume_at(kTicket,kUsb);}
}
