// Isolated raw NUP selected-union extractor pilot. No console deployment path.
// Uses the Eglantine self-update SHA-256 implementation (GPL-3.0-or-later).
#include "self_update_sha256.h"
#include "signature_verify.hpp"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <unistd.h>
#include <vector>

namespace {
bool g_signed_job=false;
std::string g_entry_id(64,'0');
std::string g_generation(64,'0');
std::string g_stage_name;
int g_stage_fd=-1;
constexpr size_t kChunk = 1024 * 1024;
constexpr size_t kMaxManifest = 64 * 1024 * 1024;
constexpr size_t kMaxLine = 512;
struct Row { uint64_t off, stored, size; unsigned codec; std::string sha, path; };
bool safe_component(const std::string &s);
bool exact_tree(int dir,const std::string &prefix,const std::set<std::string> &files,const std::set<std::string> &dirs) {
    DIR *stream=fdopendir(dup(dir)); if(!stream)return false;
    bool ok=true;struct dirent *entry;
    while((entry=readdir(stream))!=nullptr && ok) {
        std::string name=entry->d_name;
        if(name=="."||name=="..")continue;
        if(prefix.empty() && (name==".checkpoint"||name==".ready"||name==".verified"||name==".status"))continue;
        std::string path=prefix.empty()?name:prefix+"/"+name;
        struct stat st{};
        if(!safe_component(name)||fstatat(dir,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)!=0){ok=false;break;}
        if(S_ISDIR(st.st_mode)) {
            if(!dirs.count(path)){ok=false;break;}
            int child=openat(dir,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
            ok=child>=0&&exact_tree(child,path,files,dirs);if(child>=0)close(child);
        } else if(!S_ISREG(st.st_mode)||!files.count(path))ok=false;
    }
    closedir(stream);return ok;
}
struct Checkpoint { uint64_t count = 0, bytes = 0; bool exists = false; };

int fail(const std::string &why);
bool hex64(const std::string &s) {
    if (s.size()!=64) return false;
    for (char c:s) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    return true;
}
bool hex32(const std::string &s) {
    if (s.size()!=32) return false;
    for (char c:s) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    return true;
}
bool number(const std::string &s, uint64_t &v) {
    if (s.empty() || s.size()>20) return false;
    v=0;
    for (char c:s) { if(c<'0'||c>'9'||v>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10) return false; v=v*10+static_cast<unsigned>(c-'0'); }
    return true;
}
bool write_all(int fd, const char *p, size_t n) {
    while(n) { ssize_t w=write(fd,p,n); if(w<0&&errno==EINTR) continue; if(w<=0) return false; p+=w;n-=static_cast<size_t>(w); }
    return true;
}
bool status_write(int stage,const std::string &state,uint64_t count,uint64_t bytes) {
    if(stage<0)return false;
    std::string body="EMUHUB-NUP-STATUS-1\n"+state+"\n"+std::to_string(count)+"\n"+std::to_string(bytes)+"\n";
    int fd=openat(stage,".status.new",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0)return false;
    struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&write_all(fd,body.data(),body.size())&&fsync(fd)==0;
    close(fd);
    if(ok)ok=renameat(stage,".status.new",stage,".status")==0&&fsync(stage)==0;
    return ok;
}
int fail(const std::string &why) {
    if(g_stage_fd>=0)status_write(g_stage_fd,"ERROR:"+why,0,0);
    std::fprintf(stderr,"STOPPED %s\n",why.c_str());return 2;
}
bool read_exact_at(int fd, void *p, size_t n, uint64_t off) {
    char *out=static_cast<char*>(p);
    while(n) { ssize_t got=pread(fd,out,n,static_cast<off_t>(off)); if(got<0&&errno==EINTR) continue; if(got<=0) return false; out+=got;n-=static_cast<size_t>(got);off+=static_cast<size_t>(got); }
    return true;
}
uint64_t le64(const unsigned char *p) { uint64_t v=0;for(int i=7;i>=0;--i) v=(v<<8)|p[i];return v; }
unsigned le16(const unsigned char *p) { return unsigned(p[0])|(unsigned(p[1])<<8); }
std::string digest_range(int fd, uint64_t off, uint64_t size) {
    std::vector<char> buf(kChunk); self_update_sha256 h;self_update_sha256_start(&h);
    while(size) { size_t n=static_cast<size_t>(std::min<uint64_t>(size,buf.size())); if(!read_exact_at(fd,buf.data(),n,off)) return {};
        self_update_sha256_add(&h,buf.data(),n);off+=n;size-=n; }
    unsigned char raw[32];char hex[65];self_update_sha256_finish(&h,raw);self_update_sha256_hex(raw,hex);return hex;
}
bool safe_component(const std::string &s) {
    if(s.empty()||s=="."||s==".."||s.size()>255) return false;
    for(unsigned char c:s) if(c<32||c==127||c=='/'||c=='\\'||c==':') return false;
    return true;
}
bool safe_path(const std::string &s) {
    if(s.empty()||s.size()>240||s[0]=='/'||s.back()=='/') return false;
    size_t begin=0;
    while(begin<s.size()) { size_t end=s.find('/',begin);if(end==std::string::npos) end=s.size();
        if(!safe_component(s.substr(begin,end-begin))) return false;
        begin=end+1; }
    return true;
}
std::vector<std::string> lines(const std::string &s) {
    std::vector<std::string> out;if(s.empty()||s.back()!='\n') return out;
    size_t pos=0;while(pos<s.size()) { size_t end=s.find('\n',pos);if(end==std::string::npos||end-pos>kMaxLine) {out.clear();return out;}
        out.push_back(s.substr(pos,end-pos));pos=end+1; }return out;
}
std::vector<std::string> tabs(const std::string &s) {
    std::vector<std::string> out;size_t pos=0;
    while(true){size_t end=s.find('\t',pos);if(end==std::string::npos){out.push_back(s.substr(pos));break;}out.push_back(s.substr(pos,end-pos));pos=end+1;}return out;
}
bool read_small_at(int dir, const char *name, std::string &out, size_t max) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW);if(fd<0) return false;
    struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0&&static_cast<uint64_t>(st.st_size)<=max;
    if(ok) {out.resize(static_cast<size_t>(st.st_size));ok=read_exact_at(fd,out.data(),out.size(),0);}
    close(fd);return ok;
}
int walk_dir(int root,const std::string &path,bool create) {
    int cur=dup(root);if(cur<0)return -1;size_t begin=0;
    while(begin<path.size()) {size_t end=path.find('/',begin);if(end==std::string::npos)end=path.size();
        std::string part=path.substr(begin,end-begin);if(!safe_component(part)){close(cur);return -1;}
        if(create&&mkdirat(cur,part.c_str(),0700)!=0&&errno!=EEXIST){close(cur);return -1;}
        int next=openat(cur,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(cur);if(next<0)return -1;cur=next;begin=end+1; }
    return cur;
}
bool marker_ok(int root,const std::string &id) {
    int d=walk_dir(root,"homebrew/EmuHub/PPSA91337",false);if(d<0)return false;
    std::string got;bool ok=read_small_at(d,"storage-id.txt",got,128);close(d);
    return ok&&got=="emuhub-ps5-storage-v1\nPPSA91337\n"+id+"\n";
}
bool free_ok(const std::string &root,uint64_t need,uint64_t reserve) {
    struct statfs fs{};if(statfs(root.c_str(),&fs)!=0||fs.f_bsize<=0)return false;
    uint64_t blocks=static_cast<uint64_t>(fs.f_bavail),unit=static_cast<uint64_t>(fs.f_bsize);
    if(blocks>UINT64_MAX/unit||need>UINT64_MAX-reserve)return false;
    return blocks*unit>=need+reserve;
}
bool checkpoint_write(int stage,const std::string &manifest_sha,const std::string &source_sha,const std::string &marker,uint64_t count,uint64_t bytes) {
    std::string body="EMUHUB-NUP-CHECKPOINT-1\n"+manifest_sha+"\n"+source_sha+"\n"+marker+"\n"+std::to_string(count)+"\n"+std::to_string(bytes)+"\n";
    int fd=openat(stage,".checkpoint.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return false;
    bool ok=write_all(fd,body.data(),body.size())&&fsync(fd)==0;close(fd);if(!ok)return false;
    if(renameat(stage,".checkpoint.new",stage,".checkpoint")!=0)return false;
    return fsync(stage)==0;
}
bool checkpoint_read(int stage,const std::string &manifest_sha,const std::string &source_sha,const std::string &marker,Checkpoint &cp) {
    std::string body;if(!read_small_at(stage,".checkpoint",body,512))return false;
    auto l=lines(body);if(l.size()!=6||l[0]!="EMUHUB-NUP-CHECKPOINT-1"||l[1]!=manifest_sha||l[2]!=source_sha||l[3]!=marker)return false;
    cp.exists=number(l[4],cp.count)&&number(l[5],cp.bytes);return cp.exists;
}
bool cancel_requested(int stage,const std::string &manifest_sha) {
    if(!g_signed_job)return false;
    std::string request;
    if(!read_small_at(stage,".cancel",request,448))return false;
    const std::string body="EMUHUB-NUP-CANCEL-2\n"+g_entry_id+"\n"+g_generation+"\n"+manifest_sha+"\n";
    if(request.size()!=body.size()+129||request.compare(0,body.size(),body)!=0||request.back()!='\n')return false;
    return emuhub_signature::verify(body,request.substr(body.size(),128));
}
bool file_at_path(int stage,const Row &row,bool verify_only,int source) {
    size_t cut=row.path.rfind('/');std::string dirpath=cut==std::string::npos?"":row.path.substr(0,cut);
    std::string leaf=cut==std::string::npos?row.path:row.path.substr(cut+1);
    int parent=dirpath.empty()?dup(stage):walk_dir(stage,dirpath,!verify_only);if(parent<0)return false;
    int fd=openat(parent,leaf.c_str(),(verify_only?O_RDONLY:O_WRONLY|O_CREAT|O_EXCL)|O_NOFOLLOW,0600);
    if(fd<0){
        int saved=errno;close(parent);
        if(!verify_only&&saved==EEXIST)return file_at_path(stage,row,true,source);
        return false;
    }
    struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode);
    if(verify_only)ok=ok&&static_cast<uint64_t>(st.st_size)==row.size;
    self_update_sha256 hash;self_update_sha256_start(&hash);std::vector<char> buffer(kChunk);uint64_t left=row.size,off=row.off;
    while(ok&&left){size_t n=static_cast<size_t>(std::min<uint64_t>(left,buffer.size()));
        if(verify_only)ok=read_exact_at(fd,buffer.data(),n,row.size-left);
        else ok=read_exact_at(source,buffer.data(),n,off)&&write_all(fd,buffer.data(),n);
        if(ok)self_update_sha256_add(&hash,buffer.data(),n);
        left-=n;off+=n;}
    unsigned char digest[32];char hex[65];self_update_sha256_finish(&hash,digest);self_update_sha256_hex(digest,hex);
    ok=ok&&row.sha==hex;
    if(ok&&!verify_only)ok=fsync(fd)==0;
    close(fd);if(ok&&!verify_only)ok=fsync(parent)==0;close(parent);return ok;
}
int execute(int argc,char **argv) {
    if(argc!=11)return fail("USAGE source.nup sidecar.txt usb_root stage_name marker_id source_sha toc_sha manifest_sha reserve_bytes stop_after");
    const std::string source_path=argv[1],manifest_path=argv[2],usb_root=argv[3],stage_name=argv[4],marker_id=argv[5];
    const std::string source_sha=argv[6],toc_sha=argv[7],manifest_sha=argv[8];uint64_t reserve=0,stop_after=0;
    g_stage_name=stage_name;
    if(!safe_component(stage_name)||!hex32(marker_id)||!hex64(source_sha)||!hex64(toc_sha)||!hex64(manifest_sha)||
       !number(argv[9],reserve)||!number(argv[10],stop_after))return fail("BAD_ARGUMENT");
    int root=open(usb_root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(root<0)return fail("USB_ROOT_UNSAFE_OR_ABSENT");
    if(!marker_ok(root,marker_id)){close(root);return fail("USB_MARKER_MISMATCH");}
    int src=open(source_path.c_str(),O_RDONLY|O_NOFOLLOW);if(src<0){close(root);return fail("SOURCE_OPEN");}
    struct stat srcst{};if(fstat(src,&srcst)!=0||!S_ISREG(srcst.st_mode)||srcst.st_size<128){close(src);close(root);return fail("SOURCE_STAT");}
    uint64_t source_size=static_cast<uint64_t>(srcst.st_size);
    if(digest_range(src,0,source_size)!=source_sha){close(src);close(root);return fail("SOURCE_SHA_MISMATCH");}
    unsigned char header[128];if(!read_exact_at(src,header,128,0)||std::memcmp(header,"NUP\x01",4)!=0||le16(header+4)<128||le64(header+64)!=source_size){close(src);close(root);return fail("NUP_HEADER_MISMATCH");}
    uint64_t to=le64(header+8),ts=le64(header+16),blob=le64(header+56);
    if(to<128||ts==0||ts>64*1024*1024||to>source_size||ts>source_size-to||blob<to+ts||blob>source_size||
       digest_range(src,to,ts)!=toc_sha){close(src);close(root);return fail("NUP_TOC_MISMATCH");}
    unsigned char tocraw[32];for(unsigned i=0;i<32;++i)tocraw[i]=header[24+i];char tochex[65];self_update_sha256_hex(tocraw,tochex);
    if(toc_sha!=tochex){close(src);close(root);return fail("NUP_TOC_HEADER_SHA_MISMATCH");}
    int mfd=open(manifest_path.c_str(),O_RDONLY|O_NOFOLLOW);if(mfd<0){close(src);close(root);return fail("MANIFEST_OPEN");}
    struct stat mst{};if(fstat(mfd,&mst)!=0||!S_ISREG(mst.st_mode)||mst.st_size<1||static_cast<uint64_t>(mst.st_size)>kMaxManifest){close(mfd);close(src);close(root);return fail("MANIFEST_SIZE");}
    std::string manifest;manifest.resize(static_cast<size_t>(mst.st_size));bool mread=read_exact_at(mfd,manifest.data(),manifest.size(),0);close(mfd);
    self_update_sha256 mh;self_update_sha256_start(&mh);self_update_sha256_add(&mh,manifest.data(),manifest.size());unsigned char md[32];char mhex[65];self_update_sha256_finish(&mh,md);self_update_sha256_hex(md,mhex);
    if(!mread||manifest_sha!=mhex){close(src);close(root);return fail("MANIFEST_SHA_MISMATCH");}
    auto ls=lines(manifest);if(ls.size()<5||ls[0]!="EMUHUB-NUP-SELECTED-1"||ls[1]!="source="+source_sha||ls[2]!="toc="+toc_sha){close(src);close(root);return fail("MANIFEST_HEADER");}
    uint64_t expected_count=0,expected_bytes=0;if(ls[3].substr(0,6)!="count="||!number(ls[3].substr(6),expected_count)||
       ls[4].substr(0,6)!="bytes="||!number(ls[4].substr(6),expected_bytes)||ls.size()!=expected_count+5){close(src);close(root);return fail("MANIFEST_TOTALS");}
    std::vector<Row> rows;rows.reserve(static_cast<size_t>(expected_count));std::set<std::string> seen,folded,dirs;uint64_t total=0,lastoff=0;
    for(size_t i=5;i<ls.size();++i){auto v=tabs(ls[i]);Row row{};uint64_t codec=0;
        if(v.size()!=6||!number(v[0],row.off)||!number(v[1],row.stored)||!number(v[2],row.size)||!number(v[3],codec)||
           codec!=0||!hex64(v[4])||!safe_path(v[5])||!seen.insert(v[5]).second||row.stored!=row.size||
           row.off<blob||row.off<lastoff||row.off>source_size||row.stored>source_size-row.off||total>UINT64_MAX-row.size){close(src);close(root);return fail("MANIFEST_ROW_UNSAFE_OR_UNSUPPORTED");}
        std::string fold=v[5];for(char &ch:fold)if(ch>='A'&&ch<='Z')ch=static_cast<char>(ch+32);
        if(!folded.insert(fold).second){close(src);close(root);return fail("MANIFEST_CASE_COLLISION");}
        for(size_t slash=v[5].find('/');slash!=std::string::npos;slash=v[5].find('/',slash+1))dirs.insert(v[5].substr(0,slash));
        row.codec=static_cast<unsigned>(codec);row.sha=v[4];row.path=v[5];lastoff=row.off;total+=row.size;rows.push_back(std::move(row));}
    if(total!=expected_bytes){close(src);close(root);return fail("MANIFEST_PAYLOAD_TOTAL");}
    if(!free_ok(usb_root,expected_bytes,reserve)){close(src);close(root);return fail("USB_SPACE_RESERVE");}
    // Marker remains at USB root; output is scoped to the provider's game root.
    int games=walk_dir(root,"eglantineps5/roms",false);
    if(games<0){close(src);close(root);return fail("GAME_ROOT_UNSAFE_OR_ABSENT");}
    bool made=mkdirat(games,stage_name.c_str(),0700)==0;
    if(!made&&errno!=EEXIST){close(games);close(src);close(root);return fail("STAGE_CREATE");}
    int stage=openat(games,stage_name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    close(games);close(root);
    if(stage<0){close(src);return fail("STAGE_UNSAFE");}
    g_stage_fd=dup(stage);
    if(g_stage_fd<0){close(stage);close(src);return fail("STATUS_HANDLE");}
    Checkpoint cp{};
    if(made){if(!checkpoint_write(stage,manifest_sha,source_sha,marker_id,0,0)){close(stage);close(src);return fail("CHECKPOINT_INIT");}}
    else if(!checkpoint_read(stage,manifest_sha,source_sha,marker_id,cp)){close(stage);close(src);return fail("CHECKPOINT_MISSING_OR_MISMATCH");}
    if(cp.count>rows.size()){close(stage);close(src);return fail("CHECKPOINT_COUNT");}
    uint64_t verified_bytes=0;
    for(uint64_t i=0;i<cp.count;++i){if(!file_at_path(stage,rows[static_cast<size_t>(i)],true,src)){close(stage);close(src);return fail("CHECKPOINT_FILE_READBACK");}verified_bytes+=rows[static_cast<size_t>(i)].size;}
    if(verified_bytes!=cp.bytes){close(stage);close(src);return fail("CHECKPOINT_BYTES");}
    uint64_t done=cp.count,bytes=cp.bytes;
    if(!status_write(stage,"RUNNING",done,bytes)){close(stage);close(src);return fail("STATUS_WRITE");}
    for(size_t i=static_cast<size_t>(done);i<rows.size();++i){
        if(cancel_requested(stage,manifest_sha)){
            status_write(stage,"CANCELLED",done,bytes);close(stage);close(src);return 4;
        }
        int checkroot=open(usb_root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        bool okay=checkroot>=0&&marker_ok(checkroot,marker_id)&&free_ok(usb_root,expected_bytes-bytes,reserve);
        if(checkroot>=0)close(checkroot);
        if(!okay){close(stage);close(src);return fail("USB_MARKER_OR_SPACE_CHANGED");}
        if(!file_at_path(stage,rows[i],false,src)){close(stage);close(src);return fail("MEMBER_SHA_OR_PATH_WRITE");}
        ++done;bytes+=rows[i].size;
        if(!checkpoint_write(stage,manifest_sha,source_sha,marker_id,done,bytes)){close(stage);close(src);return fail("CHECKPOINT_WRITE");}
        if((done%256)==0 && !status_write(stage,"RUNNING",done,bytes)){close(stage);close(src);return fail("STATUS_WRITE");}
        if(stop_after&&done>=stop_after){status_write(stage,"PARTIAL",done,bytes);std::printf("PARTIAL members=%llu bytes=%llu\n",(unsigned long long)done,(unsigned long long)bytes);close(stage);close(src);return 3;}
    }
    if(cancel_requested(stage,manifest_sha)){
        status_write(stage,"CANCELLED",done,bytes);close(stage);close(src);return 4;
    }
    // A completed checkpoint is only progress. Reopen every destination and reject extras.
    for(const auto &row:rows)if(!file_at_path(stage,row,true,src)){close(stage);close(src);return fail("FINAL_MEMBER_READBACK");}
    if(!exact_tree(stage,"",seen,dirs)){close(stage);close(src);return fail("FINAL_EXTRAS_OR_UNSAFE");}
    struct stat after{};bool same=fstat(src,&after)==0&&after.st_ino==srcst.st_ino&&after.st_size==srcst.st_size&&after.st_mtim.tv_sec==srcst.st_mtim.tv_sec&&after.st_mtim.tv_nsec==srcst.st_mtim.tv_nsec;
    if(!same){close(stage);close(src);return fail("SOURCE_CHANGED_DURING_EXTRACTION");}
    // .ready is an evidence receipt, not yet a provider authorization. Provider must
    // verify this against a signed catalog receipt and the exact folder/USB marker.
    const std::string receipt="EMUHUB-NUP-VERIFIED-1\n"+g_entry_id+"\n"+g_generation+"\n"+source_sha+"\n"+toc_sha+"\n"+manifest_sha+"\n"+marker_id+"\n"+g_stage_name+"\n"+std::to_string(done)+"\n"+std::to_string(bytes)+"\n";
    const std::string signed_receipt=receipt;
    const char *receipt_name=".verified";
    int ready=openat(stage,receipt_name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    bool receipt_ok=false;
    if(ready<0&&errno==EEXIST) {
        std::string prior;
        receipt_ok=read_small_at(stage,receipt_name,prior,1024)&&prior==signed_receipt;
        if(!receipt_ok && !prior.empty()) {
            // The full selected tree was read back above. A fresh scanner
            // generation can reuse it without extracting duplicate content.
            struct stat oldst{};
            if(fstatat(stage,receipt_name,&oldst,AT_SYMLINK_NOFOLLOW)==0 &&
               S_ISREG(oldst.st_mode)) {
                int next=openat(stage,".verified.new",
                    O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
                if(next>=0) {
                    receipt_ok=write_all(next,signed_receipt.data(),signed_receipt.size())&&fsync(next)==0;
                    close(next);
                    if(receipt_ok)receipt_ok=renameat(stage,".verified.new",stage,receipt_name)==0&&fsync(stage)==0;
                }
            }
        }
    } else if(ready>=0) {
        receipt_ok=write_all(ready,signed_receipt.data(),signed_receipt.size())&&fsync(ready)==0;
        close(ready);receipt_ok=receipt_ok&&fsync(stage)==0;
    }
    if(receipt_ok)receipt_ok=status_write(stage,"VERIFIED",done,bytes);
    close(stage);close(src);
    if(!receipt_ok)return fail("RECEIPT_WRITE");
    std::printf("PASS members=%llu bytes=%llu\n",(unsigned long long)done,(unsigned long long)bytes);return 0;
}
} // namespace
int run_signed_job(const char *request_path) {
    int reqfd=open(request_path,O_RDONLY|O_NOFOLLOW);if(reqfd<0)return fail("JOB_REQUEST_UNAVAILABLE");
    struct stat rs{};bool req_ok=fstat(reqfd,&rs)==0&&S_ISREG(rs.st_mode)&&rs.st_size>0&&rs.st_size<=4096;
    std::string request;if(req_ok){request.resize(static_cast<size_t>(rs.st_size));req_ok=read_exact_at(reqfd,request.data(),request.size(),0);}close(reqfd);
    if(!req_ok)return fail("JOB_REQUEST_UNSAFE");
    auto fields=lines(request);
    if(fields.size()!=14||fields[0]!="EMUHUB-NUP-JOB-2"||!hex64(fields[1])||fields[13].size()!=128)return fail("JOB_REQUEST_FORMAT");
    auto cut=request.rfind('\n',request.size()-2);if(cut==std::string::npos)return fail("JOB_REQUEST_FORMAT");
    std::string body=request.substr(0,cut+1);
    if(!emuhub_signature::verify(body,fields[13]))return fail("JOB_AUTH_FAILED");
    if(!hex64(fields[2]))return fail("JOB_GENERATION");
    g_signed_job=true;g_entry_id=fields[1];g_generation=fields[2];
    std::vector<std::string> args={"native_nup_union_extract",fields[3],fields[4],fields[5],fields[6],fields[7],fields[8],fields[9],fields[10],fields[11],fields[12]};
    std::vector<char*> av;for(auto &arg:args)av.push_back(arg.data());
    return execute(static_cast<int>(av.size()),av.data());
}
#ifndef EMUHUB_PROVIDER_EMBED
int main(int argc,char **argv){
    (void)argv;
    if(argc==1)return run_signed_job("/data/homebrew/PPSA91337/config/ns2-prepare/request.txt");
#ifndef EM_HARDENED_TARGET
    if(argc==3&&std::strcmp(argv[1],"--job")==0)return run_signed_job(argv[2]);
    return execute(argc,argv);
#else
    return fail("JOB_ONLY_TARGET");
#endif
}

#endif // EMUHUB_PROVIDER_EMBED
