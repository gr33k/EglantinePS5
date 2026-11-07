#pragma once
#include "self_update_sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace emuhub_layout {
inline bool safe_component(std::string_view s){if(s.empty()||s=="."||s==".."||s.size()>255)return false;for(unsigned char c:s)if(c<32||c==127||c=='/'||c=='\\'||c==':')return false;return true;}
inline bool safe_path(std::string_view s){if(s.empty()||s.size()>240||s[0]=='/'||s.back()=='/')return false;size_t p=0;while(p<s.size()){size_t q=s.find('/',p);if(q==std::string_view::npos)q=s.size();if(!safe_component(s.substr(p,q-p)))return false;p=q+1;}return true;}
inline bool dec(std::string_view s,uint64_t&v){if(s.empty()||s.size()>20)return false;v=0;for(char c:s){if(c<'0'||c>'9'||v>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10)return false;v=v*10+static_cast<unsigned>(c-'0');}return true;}
inline bool hex64(std::string_view s){if(s.size()!=64)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline std::vector<std::string> lines(const std::string&s){std::vector<std::string> out;if(s.empty()||s.back()!='\n')return out;size_t p=0;while(p<s.size()){size_t q=s.find('\n',p);if(q==std::string::npos||q-p>512)return {};out.push_back(s.substr(p,q-p));p=q+1;}return out;}
inline std::vector<std::string> tabs(const std::string&s){std::vector<std::string> out;size_t p=0;while(true){size_t q=s.find('\t',p);if(q==std::string::npos){out.push_back(s.substr(p));break;}out.push_back(s.substr(p,q-p));p=q+1;}return out;}
inline std::string sha_string(const std::string&data){self_update_sha256 h;self_update_sha256_start(&h);self_update_sha256_add(&h,data.data(),data.size());unsigned char raw[32];char hex[65];self_update_sha256_finish(&h,raw);self_update_sha256_hex(raw,hex);return hex;}
inline int open_dir_path(const std::string&path){if(path.empty()||path[0]!='/')return -1;int cur=open("/",O_RDONLY|O_DIRECTORY);if(cur<0)return -1;size_t p=1;while(p<path.size()){size_t q=path.find('/',p);if(q==std::string::npos)q=path.size();auto name=path.substr(p,q-p);if(!safe_component(name)){close(cur);return -1;}int next=openat(cur,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(cur);if(next<0)return -1;cur=next;p=q+1;}return cur;}
inline int walk(int root,std::string_view path){int cur=dup(root);if(cur<0)return -1;size_t p=0;while(p<path.size()){size_t q=path.find('/',p);if(q==std::string_view::npos)q=path.size();auto name=std::string(path.substr(p,q-p));if(!safe_component(name)){close(cur);return -1;}int next=openat(cur,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(cur);if(next<0)return -1;cur=next;p=q+1;}return cur;}
inline bool hash_member(int root,const std::string&path,uint64_t size,const std::string&sha){auto pos=path.rfind('/');int parent=pos==std::string::npos?dup(root):walk(root,std::string_view(path).substr(0,pos));if(parent<0)return false;auto leaf=path.substr(pos==std::string::npos?0:pos+1);int fd=openat(parent,leaf.c_str(),O_RDONLY|O_NOFOLLOW);close(parent);if(fd<0)return false;struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&static_cast<uint64_t>(st.st_size)==size;self_update_sha256 h;self_update_sha256_start(&h);std::vector<char> buf(1024*1024);uint64_t done=0;while(ok&&done<size){size_t want=static_cast<size_t>(std::min<uint64_t>(size-done,buf.size()));ssize_t n=read(fd,buf.data(),want);if(n<=0){ok=false;break;}self_update_sha256_add(&h,buf.data(),static_cast<size_t>(n));done+=static_cast<size_t>(n);}unsigned char raw[32];char hex[65];self_update_sha256_finish(&h,raw);self_update_sha256_hex(raw,hex);close(fd);return ok&&done==size&&sha==hex;}
inline bool exact_tree(int root,const std::string&prefix,const std::set<std::string>&files,const std::set<std::string>&dirs){DIR* stream=fdopendir(dup(root));if(!stream)return false;bool ok=true;struct dirent* e;while((e=readdir(stream))&&ok){std::string name=e->d_name;if(name=="."||name=="..")continue;if(prefix.empty()&&(name==".checkpoint"||name==".status"||name==".verified"))continue;std::string path=prefix.empty()?name:prefix+"/"+name;struct stat st{};if(!safe_component(name)||fstatat(root,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)!=0){ok=false;break;}if(S_ISDIR(st.st_mode)){if(!dirs.count(path)){ok=false;break;}int child=openat(root,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);ok=child>=0&&exact_tree(child,path,files,dirs);if(child>=0)close(child);}else if(!S_ISREG(st.st_mode)||!files.count(path))ok=false;}closedir(stream);return ok;}
inline bool verify(const std::string&folder,const std::string&manifest_path,const std::string&manifest_sha,const std::string&source_sha,const std::string&toc_sha,uint64_t expected_count,uint64_t expected_bytes){
 int mf=open(manifest_path.c_str(),O_RDONLY|O_NOFOLLOW);if(mf<0)return false;struct stat ms{};bool ok=fstat(mf,&ms)==0&&S_ISREG(ms.st_mode)&&ms.st_size>0&&ms.st_size<=64*1024*1024;std::string text;if(ok){text.resize(static_cast<size_t>(ms.st_size));size_t at=0;while(at<text.size()){ssize_t n=read(mf,text.data()+at,text.size()-at);if(n<=0){ok=false;break;}at+=static_cast<size_t>(n);}}close(mf);if(!ok||sha_string(text)!=manifest_sha)return false;
 auto ls=lines(text);if(ls.size()!=expected_count+5||ls.size()<6||ls[0]!="EMUHUB-NUP-SELECTED-1"||ls[1]!="source="+source_sha||ls[2]!="toc="+toc_sha)return false;uint64_t count=0,bytes=0;if(ls[3].rfind("count=",0)!=0||!dec(std::string_view(ls[3]).substr(6),count)||count!=expected_count||ls[4].rfind("bytes=",0)!=0||!dec(std::string_view(ls[4]).substr(6),bytes)||bytes!=expected_bytes)return false;
 int root=open_dir_path(folder);if(root<0)return false;std::set<std::string> files,dirs;uint64_t total=0;for(size_t i=5;i<ls.size()&&ok;++i){auto f=tabs(ls[i]);uint64_t off=0,stored=0,size=0,codec=0;if(f.size()!=6||!dec(f[0],off)||!dec(f[1],stored)||!dec(f[2],size)||!dec(f[3],codec)||codec>1||(codec==0&&stored!=size)||!hex64(f[4])||!safe_path(f[5])||!files.insert(f[5]).second||total>UINT64_MAX-size){ok=false;break;}for(size_t slash=f[5].find('/');slash!=std::string::npos;slash=f[5].find('/',slash+1))dirs.insert(f[5].substr(0,slash));total+=size;ok=hash_member(root,f[5],size,f[4]);}
 ok=ok&&files.size()==expected_count&&total==expected_bytes&&exact_tree(root,"",files,dirs);close(root);return ok;
}
}
