#include "common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
namespace K {

const char *CY = "38;2;0;229;255", *AM = "38;2;255;179;0", *RD = "38;2;255;61;87", *DM = "38;2;107;135;148",
           *WH = "38;2;226;238;244", *GR = "38;2;80;230;140";
static bool tty() { return isatty(1); }
std::string c(const char* code, const std::string& t) { return tty() ? std::string("\x1b[") + code + "m" + t + "\x1b[0m" : t; }
void say(const std::string& t, const char* col) { std::cout << c(col ? col : WH, t) << "\n"; }
void ok(const std::string& t) { std::cout << c(GR, "[ OK ] ") << t << "\n"; }
void warn(const std::string& t) { std::cout << c(AM, "[WARN] ") << t << "\n"; }
void err(const std::string& t) { std::cout << c(RD, "[FAIL] ") << t << "\n"; }
static size_t vlen(const std::string& s) { size_t n = 0; for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) n++; return n; }
void box(const std::string& title, const std::vector<std::string>& lines, const char* col, int width) {
    col = col ? col : CY;
    int fill = std::max(1, width - (int)vlen(title) - 6);
    std::string h; for (int i = 0; i < fill; i++) h += "─";
    std::cout << c(col, "┌─[ ") << c(AM, title) << c(col, " ]" + h + "┐") << "\n";
    for (auto& l : lines) std::cout << c(col, "│ ") << l << "\n";
    std::string b; for (int i = 0; i < width - 2; i++) b += "─";
    std::cout << c(col, "└" + b + "┘") << "\n";
}
void banner() {
    std::string b; for (int i = 0; i < 62; i++) b += "━";
    std::cout << c(CY, "┏" + b + "┓") << "\n"
              << c(CY, "┃ MUX // SYSTEM KITCHEN") << c(DM, "   unpack . debloat . add apps . repack") << c(CY, "  ┃") << "\n"
              << c(CY, "┗" + b + "┛") << "\n";
}
std::string ask(const std::string& p, const std::string& def) {
    std::cout << c(AM, p + (def.empty() ? "" : " [" + def + "]") + " > ") << std::flush;
    std::string r; if (!std::getline(std::cin, r)) r = "";
    size_t a = r.find_first_not_of(" \t\r"), b = r.find_last_not_of(" \t\r");
    r = a == std::string::npos ? "" : r.substr(a, b - a + 1);
    return r.empty() ? def : r;
}
std::string human(uint64_t n) {
    const char* u[] = {"B", "KB", "MB", "GB"}; double d = (double)n; int i = 0;
    while (d >= 1024 && i < 3) { d /= 1024; i++; }
    char buf[64]; if (i == 0) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)n); else snprintf(buf, sizeof buf, "%.1f %s", d, u[i]);
    return buf;
}

// ---------------------------------------------------------------- processes / files
RunRes run(const std::vector<std::string>& argv) {
    RunRes r; int po[2]; if (pipe(po) != 0) return r;
    char tmpl[] = "/tmp/kitchen_errXXXXXX"; int ef = mkstemp(tmpl);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(po[1], 1); dup2(ef, 2); close(po[0]); close(po[1]);
        std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
        execvp(a[0], a.data()); _exit(127);
    }
    close(po[1]); char buf[65536]; ssize_t n;
    while ((n = read(po[0], buf, sizeof buf)) > 0) r.out.append(buf, (size_t)n);
    close(po[0]); int st = 0; waitpid(pid, &st, 0); r.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    lseek(ef, 0, SEEK_SET); while ((n = read(ef, buf, sizeof buf)) > 0) r.err.append(buf, (size_t)n);
    close(ef); unlink(tmpl); return r;
}
bool haveTool(const std::string& name) {
    const char* p = getenv("PATH"); std::stringstream ss(p ? p : "");
    std::string d; while (std::getline(ss, d, ':')) { std::string f = d + "/" + name; if (access(f.c_str(), X_OK) == 0) return true; }
    return false;
}
bool fileExists(const std::string& p) { std::error_code e; return fs::exists(p, e); }
uint64_t fileSize(const std::string& p) { std::error_code e; auto s = fs::file_size(p, e); return e ? 0 : (uint64_t)s; }
Bytes readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
bool writeAll(const std::string& p, const Bytes& b) { std::ofstream f(p, std::ios::binary); f.write((const char*)b.data(), (std::streamsize)b.size()); return (bool)f; }
bool writeText(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; return (bool)f; }
std::string readText(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
bool copyFile(const std::string& a, const std::string& b) { std::error_code e; return fs::copy_file(a, b, fs::copy_options::overwrite_existing, e) && !e; }
std::string makeTempDir() { char t[] = "/tmp/kitchenXXXXXX"; return mkdtemp(t) ? t : "/tmp"; }
void removeAll(const std::string& p) { std::error_code e; fs::remove_all(p, e); }
std::string baseName(const std::string& p) { size_t s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }

// ---------------------------------------------------------------- sparse
uint32_t fileMagic(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return 0; uint32_t m = 0; if (fread(&m, 4, 1, f) != 1) m = 0; fclose(f); return m; }
bool unsparse(const std::string& src, const std::string& dst, std::string& e) {
    FILE* f = fopen(src.c_str(), "rb"); FILE* o = fopen(dst.c_str(), "wb");
    if (!f || !o) { e = "cannot open files"; if (f) fclose(f); if (o) fclose(o); return false; }
    uint8_t h[28]; if (fread(h, 28, 1, f) != 1) { e = "short header"; return false; }
    uint16_t fhs, chs; uint32_t bs, tblk, tch; memcpy(&fhs, h + 8, 2); memcpy(&chs, h + 10, 2); memcpy(&bs, h + 12, 4); memcpy(&tblk, h + 16, 4); memcpy(&tch, h + 20, 4);
    (void)chs; fseeko(f, fhs, SEEK_SET); Bytes buf(1 << 22);
    for (uint32_t i = 0; i < tch; i++) {
        uint8_t ch[12]; if (fread(ch, 12, 1, f) != 1) { e = "truncated"; return false; }
        uint16_t t; uint32_t csz; memcpy(&t, ch, 2); memcpy(&csz, ch + 4, 4); uint64_t bytes = (uint64_t)csz * bs;
        if (t == 0xCAC1) { while (bytes) { size_t n = (size_t)std::min<uint64_t>(bytes, buf.size()); if (fread(buf.data(), 1, n, f) != n) { e = "truncated raw"; return false; } fwrite(buf.data(), 1, n, o); bytes -= n; } }
        else if (t == 0xCAC2) { uint8_t fill[4]; if (fread(fill, 4, 1, f) != 1) return false; for (size_t k = 0; k < buf.size(); k++) buf[k] = fill[k & 3];
            while (bytes) { size_t n = (size_t)std::min<uint64_t>(bytes, buf.size()); fwrite(buf.data(), 1, n, o); bytes -= n; } }
        else if (t == 0xCAC3) fseeko(o, (off_t)bytes, SEEK_CUR);
        else if (t == 0xCAC4) fseeko(f, 4, SEEK_CUR);
        else { e = "unknown chunk"; return false; }
    }
    fflush(o); if (ftruncate(fileno(o), (off_t)((uint64_t)tblk * bs)) != 0) { e = "truncate failed"; return false; }
    fclose(f); fclose(o); return true;
}
bool toSparse(const std::string& src, const std::string& dst) {
    const uint32_t bs = 4096; FILE* f = fopen(src.c_str(), "rb"); FILE* o = fopen(dst.c_str(), "wb"); if (!f || !o) return false;
    uint8_t zero[bs]; memset(zero, 0, bs); uint8_t hdr[28] = {0}; fwrite(hdr, 28, 1, o);
    uint32_t chunks = 0, blocks = 0; int type = 0; uint32_t runBlocks = 0; Bytes run;   // 1 raw, 2 zero
    auto flush = [&]() {
        if (!runBlocks) return;
        uint8_t ch[12] = {0}; uint32_t tsz = type == 1 ? 12 + runBlocks * bs : 12; uint16_t t = type == 1 ? 0xCAC1 : 0xCAC3;
        memcpy(ch, &t, 2); memcpy(ch + 4, &runBlocks, 4); memcpy(ch + 8, &tsz, 4); fwrite(ch, 12, 1, o);
        if (type == 1) fwrite(run.data(), 1, run.size(), o);
        chunks++; run.clear(); runBlocks = 0; type = 0;
    };
    Bytes buf(bs * 256); size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (n % bs) { memset(buf.data() + n, 0, bs - n % bs); n += bs - n % bs; }
        for (size_t i = 0; i < n; i += bs) {
            int t = memcmp(buf.data() + i, zero, bs) == 0 ? 2 : 1;
            if (t != type) { flush(); type = t; }
            runBlocks++; blocks++;
            if (t == 1) { run.insert(run.end(), buf.begin() + i, buf.begin() + i + bs); if (runBlocks >= 2048) { flush(); type = t; } }
        }
    }
    flush(); fseeko(o, 0, SEEK_SET);
    uint32_t magic = SPARSE_MAGIC; uint16_t maj = 1, mnr = 0, fhs = 28, chs = 12; uint32_t zero32 = 0;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &maj, 2); memcpy(hdr + 6, &mnr, 2); memcpy(hdr + 8, &fhs, 2); memcpy(hdr + 10, &chs, 2);
    memcpy(hdr + 12, &bs, 4); memcpy(hdr + 16, &blocks, 4); memcpy(hdr + 20, &chunks, 4); memcpy(hdr + 24, &zero32, 4);
    fwrite(hdr, 28, 1, o); fclose(f); fclose(o); return true;
}

// ---------------------------------------------------------------- ext4 / debugfs
FsInfo fsInfo(const std::string& img) {
    FsInfo r; auto o = run({"dumpe2fs", "-h", img}); std::stringstream ss(o.out); std::string l;
    while (std::getline(ss, l)) {
        size_t p = l.find(':'); if (p == std::string::npos) continue;
        std::string k = l.substr(0, p), v = l.substr(p + 1); size_t a = v.find_first_not_of(" \t"); v = a == std::string::npos ? "" : v.substr(a);
        if (k == "Block size") r.bs = strtoull(v.c_str(), nullptr, 10);
        else if (k == "Block count") r.blocks = strtoull(v.c_str(), nullptr, 10);
        else if (k == "Free blocks") r.free = strtoull(v.c_str(), nullptr, 10);
        else if (k == "Filesystem features") r.features = v;
        else if (k == "Filesystem volume name") r.name = v;
    }
    return r;
}
std::string q(const std::string& p) { std::string s = "\""; for (char ch : p) { if (ch == '"') s += '\\'; s += ch; } return s + "\""; }
DbgRes dbg(const std::string& img, const std::vector<std::string>& cmds, bool write) {
    std::string t = "/tmp/kitchen_cmdXXXXXX"; std::vector<char> tmpl(t.begin(), t.end()); tmpl.push_back(0);
    int fd = mkstemp(tmpl.data()); std::string body; for (auto& s : cmds) body += s + "\n";
    if (fd >= 0) { if (::write(fd, body.data(), body.size()) < 0) {} close(fd); }
    std::vector<std::string> a = {"debugfs"}; if (write) a.push_back("-w"); a.push_back("-f"); a.push_back(tmpl.data()); a.push_back(img);
    auto r = run(a); unlink(tmpl.data());
    DbgRes d; d.err = r.err; std::stringstream ss(r.out); std::string l;
    while (std::getline(ss, l)) {
        if (l.rfind("debugfs: ", 0) == 0) { d.blocks.emplace_back(); continue; }
        if (!d.blocks.empty()) d.blocks.back().push_back(l);
    }
    return d;
}
static std::vector<std::string> split(const std::string& s, char d) { std::vector<std::string> v; std::string cur; for (char ch : s) { if (ch == d) { v.push_back(cur); cur.clear(); } else cur += ch; } v.push_back(cur); return v; }
std::map<std::string, std::vector<Ent>> lsDir(const std::string& img, const std::vector<std::string>& paths) {
    std::vector<std::string> cmds; for (auto& p : paths) cmds.push_back("ls -p " + q(p));
    auto d = dbg(img, cmds); std::map<std::string, std::vector<Ent>> res;
    for (size_t i = 0; i < paths.size(); i++) {
        auto& ents = res[paths[i]]; if (i >= d.blocks.size()) continue;
        for (auto& l : d.blocks[i]) {
            auto f = split(l, '/'); if (f.size() < 8 || f[1].empty() || !isdigit((unsigned char)f[1][0])) continue;
            const std::string& name = f[5]; if (name == "." || name == "..") continue;
            unsigned long mode = strtoul(f[2].c_str(), nullptr, 8); uint64_t size = f[6].empty() ? 0 : strtoull(f[6].c_str(), nullptr, 10);
            unsigned type = (mode >> 12) & 0xF; char kind = type == 4 ? 'd' : (type == 10 ? 'l' : 'f'); if (type == 8) kind = 'f';
            ents.push_back({name, kind, size});
        }
    }
    return res;
}
std::map<std::string, Tree> walk(const std::string& img, const std::vector<std::string>& roots) {
    std::map<std::string, Tree> info; std::map<std::string, std::string> owner; std::vector<std::string> frontier;
    for (auto& r : roots) { info[r].dirs.push_back(r); owner[r] = r; frontier.push_back(r); }
    while (!frontier.empty()) {
        auto lst = lsDir(img, frontier); std::vector<std::string> nxt;
        for (auto& kv : lst) {
            const std::string& root = owner[kv.first];
            for (auto& e : kv.second) {
                std::string p = kv.first; while (!p.empty() && p.back() == '/') p.pop_back(); p += "/" + e.name;
                if (e.kind == 'd') { info[root].dirs.push_back(p); owner[p] = root; nxt.push_back(p); }
                else info[root].files.push_back({p, e.size});
            }
        }
        frontier = nxt;
    }
    return info;
}
bool existsIn(const std::string& img, const std::string& path) {
    auto d = dbg(img, {"stat " + q(path)}); if (d.blocks.empty()) return false;
    for (auto& l : d.blocks[0]) if (l.find("Inode:") != std::string::npos) return true;
    return false;
}
bool dumpFileIn(const std::string& img, const std::string& path, const std::string& local) {
    unlink(local.c_str()); dbg(img, {"dump " + q(path) + " " + q(local)}); return fileExists(local);
}
Bytes readFileIn(const std::string& img, const std::string& path) {
    std::string t = makeTempDir() + "/f"; Bytes b; if (dumpFileIn(img, path, t)) b = readAll(t);
    removeAll(t.substr(0, t.find_last_of('/'))); return b;
}
}
