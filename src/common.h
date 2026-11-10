#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace K {
typedef std::vector<uint8_t> Bytes;

// ---- HUD look
extern const char *CY, *AM, *RD, *DM, *WH, *GR;
std::string c(const char* code, const std::string& t);
void say(const std::string& t = "", const char* col = nullptr);
void ok(const std::string& t);
void warn(const std::string& t);
void err(const std::string& t);
void box(const std::string& title, const std::vector<std::string>& lines, const char* col = nullptr, int width = 78);
void banner();
std::string ask(const std::string& prompt, const std::string& def = "");
std::string human(uint64_t n);

// ---- processes / files
struct RunRes { int rc = -1; std::string out, err; };
RunRes run(const std::vector<std::string>& argv);
bool haveTool(const std::string& name);
bool fileExists(const std::string& p);
uint64_t fileSize(const std::string& p);
Bytes readAll(const std::string& p);
bool writeAll(const std::string& p, const Bytes& b);
bool writeText(const std::string& p, const std::string& s);
std::string readText(const std::string& p);
bool copyFile(const std::string& a, const std::string& b);
std::string makeTempDir();
void removeAll(const std::string& p);
std::string baseName(const std::string& p);

// ---- sparse images
uint32_t fileMagic(const std::string& p);
const uint32_t SPARSE_MAGIC = 0xED26FF3A;
bool unsparse(const std::string& src, const std::string& dst, std::string& e);
bool toSparse(const std::string& src, const std::string& dst);

// ---- ext4 through debugfs (no root, no mount)
struct FsInfo { uint64_t bs = 4096, blocks = 0, free = 0; std::string features, name; };
FsInfo fsInfo(const std::string& img);
struct DbgRes { std::vector<std::vector<std::string>> blocks; std::string err; };
DbgRes dbg(const std::string& img, const std::vector<std::string>& cmds, bool write = false);
std::string q(const std::string& p);
struct Ent { std::string name; char kind; uint64_t size; };
std::map<std::string, std::vector<Ent>> lsDir(const std::string& img, const std::vector<std::string>& paths);
struct Tree { std::vector<std::pair<std::string, uint64_t>> files; std::vector<std::string> dirs; };
std::map<std::string, Tree> walk(const std::string& img, const std::vector<std::string>& roots);
bool existsIn(const std::string& img, const std::string& path);
Bytes readFileIn(const std::string& img, const std::string& path);
bool dumpFileIn(const std::string& img, const std::string& path, const std::string& local);
}
