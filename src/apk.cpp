#include "apk.h"
#include <zlib.h>
#include <cstring>

namespace K {
static uint16_t u16(const Bytes& d, size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) memcpy(&v, d.data() + o, 2); return v; }
static uint32_t u32(const Bytes& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; }

bool Zip::open(const std::string& path) {
    data = readAll(path); if (data.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = data.size() - 22 + 1; i-- > 0;) { if (u32(data, i) == 0x06054b50) { eocd = i; break; } if (data.size() - i > 70000) break; }
    if (eocd == std::string::npos) return false;
    uint16_t n = u16(data, eocd + 10); size_t p = u32(data, eocd + 16);
    for (uint16_t i = 0; i < n; i++) {
        if (u32(data, p) != 0x02014b50) return false;
        E e; e.method = u16(data, p + 10); e.csize = u32(data, p + 20); e.usize = u32(data, p + 24); e.off = u32(data, p + 42);
        uint16_t nl = u16(data, p + 28), xl = u16(data, p + 30), cl = u16(data, p + 32);
        if (p + 46 + nl > data.size()) return false;
        ents[std::string((const char*)data.data() + p + 46, nl)] = e; p += 46 + nl + xl + cl;
    }
    return true;
}
std::vector<std::string> Zip::names() const { std::vector<std::string> v; for (auto& kv : ents) v.push_back(kv.first); return v; }
bool Zip::read(const std::string& name, Bytes& out) const {
    auto it = ents.find(name); if (it == ents.end()) return false;
    const E& e = it->second; if (u32(data, e.off) != 0x04034b50) return false;
    size_t start = e.off + 30 + u16(data, e.off + 26) + u16(data, e.off + 28);
    if (start + e.csize > data.size()) return false;
    if (e.method == 0) { out.assign(data.begin() + start, data.begin() + start + e.csize); return true; }
    if (e.method != 8) return false;
    out.assign(e.usize, 0); z_stream z{}; if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(data.data() + start); z.avail_in = e.csize; z.next_out = out.data(); z.avail_out = e.usize;
    int r = inflate(&z, Z_FINISH); inflateEnd(&z); return r == Z_STREAM_END;
}

// Minimal Android binary XML reader: package / sharedUserId / uses-library / uses-permission.
Manifest parseManifest(const std::string& apkPath) {
    Manifest m; Zip z; Bytes d;
    if (!z.open(apkPath) || !z.read("AndroidManifest.xml", d) || d.size() < 16) return m;
    size_t pos = u16(d, 2);
    if (u16(d, pos) != 0x0001) return m;
    uint32_t cnt = u32(d, pos + 8), flags = u32(d, pos + 16), sstart = u32(d, pos + 20), psz = u32(d, pos + 4);
    std::vector<std::string> strs;
    for (uint32_t i = 0; i < cnt; i++) {
        size_t p = pos + sstart + u32(d, pos + 28 + 4 * i); if (p >= d.size()) { strs.push_back(""); continue; }
        if (flags & 0x100) {
            uint8_t l = d[p]; p += 1 + ((l & 0x80) ? 1 : 0);
            size_t bl = d[p++]; if (bl & 0x80) { bl = ((bl & 0x7F) << 8) | d[p++]; }
            strs.push_back(std::string((const char*)d.data() + p, std::min(bl, d.size() - p)));
        } else {
            size_t l = u16(d, p); p += 2; if (l & 0x8000) { l = ((l & 0x7FFF) << 16) | u16(d, p); p += 2; }
            std::string s; for (size_t k = 0; k < l && p + 2 * k + 1 < d.size(); k++) { uint16_t ch = u16(d, p + 2 * k); s += ch < 0x80 ? (char)ch : '?'; }
            strs.push_back(s);
        }
    }
    auto S = [&](uint32_t i) { return i < strs.size() ? strs[i] : std::string(); };
    pos += psz;
    while (pos + 8 <= d.size()) {
        uint16_t t = u16(d, pos); uint32_t sz = u32(d, pos + 4);
        if (t == 0x0102) {
            std::string name = S(u32(d, pos + 20)); uint16_t astart = u16(d, pos + 24), acnt = u16(d, pos + 28); size_t a = pos + 16 + astart;
            std::map<std::string, std::string> at;
            for (uint16_t i = 0; i < acnt; i++) {
                size_t o = a + (size_t)i * 20; if (o + 20 > d.size()) break;
                std::string an = S(u32(d, o + 4)); uint32_t raw = u32(d, o + 8), dv = u32(d, o + 16); uint8_t dt = d[o + 15];
                at[an] = raw != 0xFFFFFFFF ? S(raw) : (dt == 3 ? S(dv) : std::to_string(dv));
            }
            if (name == "manifest") { m.package = at["package"]; m.shared = at["sharedUserId"]; }
            else if (name == "uses-library" && !at["name"].empty()) m.libs.push_back(at["name"]);
            else if (name == "uses-permission" && !at["name"].empty()) m.perms.push_back(at["name"]);
        }
        if (sz == 0) break;
        pos += sz;
    }
    m.ok = !m.package.empty(); return m;
}
}
