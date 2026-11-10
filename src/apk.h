#pragma once
#include "common.h"

namespace K {
struct Manifest { bool ok = false; std::string package, shared; std::vector<std::string> libs, perms; };

struct Zip {
    Bytes data;
    struct E { uint16_t method; uint32_t csize, usize, off; };
    std::map<std::string, E> ents;
    bool open(const std::string& path);
    bool has(const std::string& n) const { return ents.count(n) > 0; }
    bool read(const std::string& n, Bytes& out) const;
    std::vector<std::string> names() const;
};
Manifest parseManifest(const std::string& apkPath);
}
