// MUX // SYSTEM KITCHEN - unpack, debloat, add apps to and repack an Android ext4 system.img (WSL Ubuntu / Linux).
// No root, no mount: only e2fsprogs (debugfs, e2fsck, resize2fs, dumpe2fs).
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include "apk.h"
#include "common.h"

using namespace K;
static bool g_cli = false;   // non-interactive mode (used by the GUI)

// ------------------------------------------------------------------ knowledge
static const std::set<std::string> RISKY_NAMES = {"android", "com.android.systemui", "com.android.phone", "com.android.settings",
    "com.android.providers.settings", "com.android.providers.telephony", "com.android.providers.contacts", "com.android.providers.media",
    "com.android.providers.downloads", "com.android.packageinstaller", "com.google.android.packageinstaller", "com.android.shell",
    "com.android.bluetooth", "com.android.nfc", "com.android.server.telecom", "com.android.permissioncontroller",
    "com.google.android.permissioncontroller", "com.google.android.gms", "com.google.android.gsf", "com.android.vending",
    "com.android.webview", "com.google.android.webview", "com.android.keychain", "com.android.inputdevices", "com.android.certinstaller",
    "com.android.location.fused", "com.android.stk", "com.android.mms.service", "com.android.networkstack", "com.android.captiveportallogin",
    "com.android.storagemanager", "com.android.externalstorage", "com.android.documentsui", "com.qualcomm.qti.services.systemhelper",
    "com.qualcomm.qcrilmsgtunnel", "org.codeaurora.ims", "com.android.carrierconfig", "com.android.modulemetadata",
    "com.google.android.ext.services", "com.google.android.ext.shared"};
static const char* RISKY_PREFIX[] = {"com.android.inputmethod", "com.google.android.inputmethod", "com.android.launcher",
    "com.google.android.apps.nexuslauncher", "com.android.theme", "android.overlay", "com.android.internal", "com.android.overlay",
    "com.android.wifi", "com.android.localtransport"};
static const std::set<std::string> RISKY_DIRS = {"SystemUI", "Settings", "SettingsProvider", "TeleService", "Telecom", "TelephonyProvider",
    "ContactsProvider", "MediaProvider", "DownloadProvider", "PackageInstaller", "PermissionController", "Shell", "Bluetooth", "NfcNci",
    "GmsCore", "PrebuiltGmsCore", "GoogleServicesFramework", "Phonesky", "Vending", "WebView", "GoogleWebView", "KeyChain", "InputDevices",
    "CertInstaller", "FusedLocation", "NetworkStack", "CaptivePortalLogin", "StorageManager", "ExternalStorageProvider", "DocumentsUI",
    "CarrierConfig", "ExtServices", "ExtShared", "framework-res", "Launcher3", "Launcher3QuickStep"};
static bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
static bool isRisky(const std::string& pkg, const std::string& dir, const std::string& shared) {
    if (!pkg.empty()) { if (RISKY_NAMES.count(pkg)) return true; for (auto p : RISKY_PREFIX) if (startsWith(pkg, p)) return true; }
    if (startsWith(shared, "android.uid.")) return true;
    return RISKY_DIRS.count(dir) > 0;
}

// ------------------------------------------------------------------ project state
struct Project {
    std::string root, img, stateF;
    uint64_t orig_bytes = 0, orig_blocks = 0, bs = 4096;
    std::string prefix;
    std::vector<std::string> removed, added;
    explicit Project(const std::string& r) {
        root = r; system(("mkdir -p '" + r + "/input' '" + r + "/work' '" + r + "/out' '" + r + "/apps'").c_str());
        img = root + "/work/system.raw.img"; stateF = root + "/project.txt"; load();
    }
    void load() {
        std::stringstream ss(readText(stateF)); std::string l;
        while (std::getline(ss, l)) {
            size_t e = l.find('='); if (e == std::string::npos) continue; std::string k = l.substr(0, e), v = l.substr(e + 1);
            if (k == "orig_bytes") orig_bytes = strtoull(v.c_str(), nullptr, 10);
            else if (k == "orig_blocks") orig_blocks = strtoull(v.c_str(), nullptr, 10);
            else if (k == "bs") bs = strtoull(v.c_str(), nullptr, 10);
            else if (k == "prefix") prefix = v; else if (k == "removed") removed.push_back(v); else if (k == "added") added.push_back(v);
        }
    }
    void save() {
        std::string s = "orig_bytes=" + std::to_string(orig_bytes) + "\norig_blocks=" + std::to_string(orig_blocks) + "\nbs=" + std::to_string(bs) + "\nprefix=" + prefix + "\n";
        for (auto& r : removed) s += "removed=" + r + "\n"; for (auto& a : added) s += "added=" + a + "\n";
        writeText(stateF, s);
    }
    bool have() const { return fileExists(img) && orig_bytes > 0; }
};
static std::vector<std::string> appDirs(const std::string& p) {
    return {p + "/app", p + "/priv-app", p + "/product/app", p + "/product/priv-app", p + "/system_ext/app", p + "/system_ext/priv-app"};
}

// ------------------------------------------------------------------ actions
static void actInfo(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    auto i = fsInfo(P.img); std::vector<std::string> L;
    L.push_back("original image : " + human(P.orig_bytes) + "  (" + std::to_string(P.orig_blocks) + " blocks)");
    L.push_back("working fs     : " + human(i.blocks * i.bs) + "  free " + human(i.free * i.bs));
    L.push_back("features       : " + i.features.substr(0, 60)); L.push_back("app root       : '" + (P.prefix.empty() ? "/" : P.prefix) + "'");
    L.push_back("removed so far : " + std::to_string(P.removed.size()) + "   added so far: " + std::to_string(P.added.size()));
    auto bp = readFileIn(P.img, P.prefix + "/build.prop"); std::stringstream ss(std::string(bp.begin(), bp.end())); std::string l;
    while (std::getline(ss, l)) for (auto k : {"ro.build.display.id", "ro.build.version.release", "ro.build.fingerprint", "ro.product.model"})
        if (startsWith(l, std::string(k) + "=")) { char b[64]; snprintf(b, sizeof b, "%-26s", k); L.push_back(b + l.substr(strlen(k) + 1)); }
    box("IMAGE INFO", L);
}

static void actUnpack(Project& P, std::string src = "", int growMb = -1) {
    if (src.empty()) src = ask("path to stock system.img (sparse or raw)");
    if (!src.empty() && src[0] == '~') src = std::string(getenv("HOME") ? getenv("HOME") : "") + src.substr(1);
    if (!fileExists(src)) { err("file not found"); return; }
    say("converting to raw ext4 ...", DM);
    if (!fileExists(P.root + "/input/system.img")) copyFile(src, P.root + "/input/system.img");
    if (fileMagic(src) == SPARSE_MAGIC) { std::string e; if (!unsparse(src, P.img, e)) { err("unsparse failed: " + e); return; } }
    else copyFile(src, P.img);
    Bytes head = readAll(P.img); if (head.size() < 1082 || head[1080] != 0x53 || head[1081] != 0xEF) {
        err("not an ext4 image (EROFS / super.img? this tool handles ext4 system.img only)"); removeAll(P.img); return; }
    run({"e2fsck", "-fy", P.img}); auto info = fsInfo(P.img);
    P.orig_bytes = fileSize(P.img); P.orig_blocks = info.blocks; P.bs = info.bs; P.removed.clear(); P.added.clear();
    bool shared = info.features.find("shared_blocks") != std::string::npos;
    if (growMb < 0) growMb = g_cli ? (shared ? 800 : 256) : atoi(ask("extra free space to add while editing (MB, shrunk back on repack)", shared ? "800" : "256").c_str());
    if (growMb > 0) {
        if (truncate(P.img.c_str(), (off_t)(P.orig_bytes + (uint64_t)growMb * 1024 * 1024)) != 0) { err("cannot grow image"); return; }
        auto r = run({"resize2fs", P.img}); if (r.rc != 0) { err("resize2fs failed:\n" + r.err + r.out); return; }
    }
    if (shared) {
        say("filesystem has shared_blocks (read-only dedup) -> unsharing ...", AM);
        auto r = run({"e2fsck", "-fy", "-E", "unshare_blocks", P.img});
        if (r.rc != 0 && r.rc != 1) { err("unshare failed, increase the extra space:\n" + r.out.substr(r.out.size() > 400 ? r.out.size() - 400 : 0)); return; }
        if (fsInfo(P.img).features.find("shared_blocks") != std::string::npos) run({"debugfs", "-w", "-R", "feature ^shared_blocks", P.img});
        if (fsInfo(P.img).features.find("shared_blocks") != std::string::npos) { err("could not clear the shared_blocks flag"); return; }
    }
    run({"e2fsck", "-fn", P.img});
    P.prefix = existsIn(P.img, "/system/build.prop") ? "/system" : ""; P.save();
    ok("unpacked. app root = '" + (P.prefix.empty() ? std::string("/") : P.prefix) + "'  fs " + human(info.blocks * info.bs)); actInfo(P);
}

struct AppItem {
    std::string path, name, pkg, shared, why; uint64_t size = 0; bool isdir = false, risky = false;
    std::vector<std::string> libs, dirs, files, mates, neededby, needs;
};
static std::string join(const std::vector<std::string>& v, const std::string& sep, size_t cap = 100) {
    std::string s; for (size_t i = 0; i < v.size() && i < cap; i++) { if (i) s += sep; s += v[i]; } return s;
}
static std::vector<std::string> splitStr(const std::string& s, char d) { std::vector<std::string> v; std::string cur; for (char ch : s) { if (ch == d) { v.push_back(cur); cur.clear(); } else cur += ch; } v.push_back(cur); return v; }

static std::vector<AppItem> scanApps(Project& P) {
    std::vector<std::string> dirs; for (auto& d : appDirs(P.prefix)) if (existsIn(P.img, d)) dirs.push_back(d);
    std::vector<AppItem> items; if (dirs.empty()) return items;
    auto lst = lsDir(P.img, dirs);
    struct R { std::string dir, name; char kind; uint64_t size; }; std::map<std::string, R> roots; std::vector<std::string> dirroots;
    for (auto& d : dirs) for (auto& e : lst[d]) { std::string p = d + "/" + e.name; roots[p] = {d, e.name, e.kind, e.size}; if (e.kind == 'd') dirroots.push_back(p); }
    auto tree = dirroots.empty() ? std::map<std::string, Tree>() : walk(P.img, dirroots);
    // manifest cache: apk \t package \t shared \t libs(;)
    std::string cacheF = P.root + "/work/manifests.tsv"; std::map<std::string, Manifest> mc;
    { std::stringstream ss(readText(cacheF)); std::string l; while (std::getline(ss, l)) { auto f = splitStr(l, '\t'); if (f.size() < 4) continue;
        Manifest m; m.ok = !f[1].empty(); m.package = f[1]; m.shared = f[2]; for (auto& x : splitStr(f[3], ';')) if (!x.empty()) m.libs.push_back(x); mc[f[0]] = m; } }
    std::string tmpd = makeTempDir();
    for (auto& kv : roots) {
        const R& r = kv.second; AppItem it; it.path = kv.first; it.name = r.name; it.isdir = r.kind == 'd';
        std::vector<std::string> apks;
        if (it.isdir) { auto& t = tree[kv.first]; it.dirs = t.dirs; for (auto& f : t.files) { it.files.push_back(f.first); it.size += f.second;
            if (f.first.size() > 4 && f.first.substr(f.first.size() - 4) == ".apk" && std::count(f.first.begin(), f.first.end(), '/') == std::count(kv.first.begin(), kv.first.end(), '/') + 1) apks.push_back(f.first); } }
        else { it.files.push_back(kv.first); it.size = r.size; if (kv.first.size() > 4 && kv.first.substr(kv.first.size() - 4) == ".apk") apks.push_back(kv.first); }
        if (!apks.empty()) {
            Manifest m; auto c = mc.find(apks[0]);
            if (c != mc.end()) m = c->second;
            else { std::string tmp = tmpd + "/a.apk"; if (dumpFileIn(P.img, apks[0], tmp)) m = parseManifest(tmp); mc[apks[0]] = m; }
            it.pkg = m.package; it.shared = m.shared; it.libs = m.libs;
        }
        items.push_back(it);
    }
    removeAll(tmpd);
    { std::string s; for (auto& kv : mc) s += kv.first + "\t" + kv.second.package + "\t" + kv.second.shared + "\t" + join(kv.second.libs, ";") + "\n"; writeText(cacheF, s); }
    std::map<std::string, std::vector<std::string>> uidg; for (auto& it : items) if (!it.shared.empty()) uidg[it.shared].push_back(it.name);
    for (auto& it : items) {
        it.risky = isRisky(it.pkg, it.name, it.shared);
        if (!it.shared.empty()) for (auto& n : uidg[it.shared]) if (n != it.name) it.mates.push_back(n);
    }
    for (auto& it : items) {
        std::set<std::string> nb; for (auto& o : items) if (&o != &it && !it.pkg.empty() && std::find(o.libs.begin(), o.libs.end(), it.pkg) != o.libs.end()) nb.insert(o.name);
        it.neededby.assign(nb.begin(), nb.end());
        std::set<std::string> nd; for (auto& o : items) if (&o != &it && !o.pkg.empty() && std::find(it.libs.begin(), it.libs.end(), o.pkg) != it.libs.end()) nd.insert(o.name);
        it.needs.assign(nd.begin(), nd.end());
        if (!it.neededby.empty()) { it.risky = true; it.why = "needed by " + join(it.neededby, ", ", 4); }
        if (startsWith(it.shared, "android.uid.")) { if (!it.why.empty()) it.why += " "; it.why += "shared system UID " + it.shared; }
    }
    return items;
}
static std::vector<int> parseSel(const std::string& txt, int n) {
    std::set<int> sel; for (auto& part : splitStr([&] { std::string s = txt; for (auto& ch : s) if (ch == ' ') ch = ','; return s; }(), ',')) {
        if (part.empty()) continue; size_t d = part.find('-');
        if (d != std::string::npos && d > 0) { int a = atoi(part.substr(0, d).c_str()), b = atoi(part.substr(d + 1).c_str()); for (int i = a; i <= b; i++) sel.insert(i); }
        else if (isdigit((unsigned char)part[0])) sel.insert(atoi(part.c_str()));
    }
    std::vector<int> r; for (int i : sel) if (i >= 1 && i <= n) r.push_back(i); return r;
}
static std::string padR(const std::string& s, size_t w) { std::string t = s.substr(0, w); while (t.size() < w) t += ' '; return t; }
static std::string padL(const std::string& s, size_t w) { std::string t = s; while (t.size() < w) t = " " + t; return t; }

static void actDelete(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    say("scanning apps (reads every APK manifest the first time, please wait) ...", DM);
    auto items = scanApps(P); std::string flt = "all";
    while (true) {
        std::vector<AppItem*> view; for (auto& it : items) if (flt == "all" || (flt == "safe" && !it.risky) || (flt == "risky" && it.risky)) view.push_back(&it);
        std::sort(view.begin(), view.end(), [](AppItem* a, AppItem* b) { return a->size > b->size; });
        std::vector<std::string> lines;
        for (size_t i = 0; i < view.size(); i++) { auto* it = view[i];
            lines.push_back(c(AM, padL(std::to_string(i + 1), 3)) + " " + (it->risky ? c(RD, "RISKY") : c(GR, "SAFE ")) + " " + c(WH, padR(it->name, 28)) + " " +
                            c(DM, padL(human(it->size), 9)) + "  " + c(DM, it->pkg.empty() ? "?" : it->pkg.substr(0, 34))); }
        if (lines.empty()) lines.push_back("(none)");
        box("APPS (" + flt + ") " + std::to_string(view.size()), lines);
        say("select: numbers/ranges (e.g. 3,5,10-12)  |  f safe / f risky / f all  |  d <n> details  |  empty = back", DM);
        std::string r = ask("delete"); if (r.empty()) return;
        if (startsWith(r, "f ")) { flt = r.substr(2); continue; }
        if (startsWith(r, "d ")) { for (int n : parseSel(r.substr(2), (int)view.size())) { auto* it = view[n - 1];
            box(it->name, {"path      : " + it->path, "package   : " + it->pkg, "shared UID: " + it->shared + "  mates: " + (it->mates.empty() ? "-" : join(it->mates, ", ")),
                           "uses libs : " + (it->libs.empty() ? "-" : join(it->libs, ", ", 6)), "needed by : " + (it->neededby.empty() ? "-" : join(it->neededby, ", ")),
                           "note      : " + (it->why.empty() ? "-" : it->why)}, AM); } continue; }
        std::vector<AppItem*> pick; for (int n : parseSel(r, (int)view.size())) pick.push_back(view[n - 1]);
        if (pick.empty()) continue;
        std::vector<std::string> rk; uint64_t tot = 0; for (auto* p : pick) { if (p->risky) rk.push_back(p->name); tot += p->size; }
        if (!rk.empty()) warn("RISKY selected: " + join(rk, ", ") + "  (may bootloop / break calls, keyboard, Play services)");
        if (ask("delete " + std::to_string(pick.size()) + " app(s), " + human(tot) + "? type yes", "no") != "yes") continue;
        std::vector<std::string> cmds;
        for (auto* p : pick) { for (auto& f : p->files) cmds.push_back("rm " + q(f));
            auto d = p->dirs; std::sort(d.begin(), d.end(), [](const std::string& a, const std::string& b) { return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/'); });
            for (auto& x : d) cmds.push_back("rmdir " + q(x)); }
        dbg(P.img, cmds, true);
        std::set<std::string> gone; for (auto* p : pick) { P.removed.push_back(p->path); gone.insert(p->path); } P.save();
        std::string log; for (auto& x : P.removed) log += x + "\n"; writeText(P.root + "/out/removed_apps.txt", log);
        ok("removed " + std::to_string(pick.size()) + " app(s)");
        items.erase(std::remove_if(items.begin(), items.end(), [&](const AppItem& a) { return gone.count(a.path) > 0; }), items.end());
    }
}

static const char CTX[] = "u:object_r:system_file:s0";
static void setMeta(std::vector<std::string>& cmds, const std::string& path, const std::string& mode, const std::string& ctxf) {
    cmds.push_back("set_inode_field " + q(path) + " mode " + mode); cmds.push_back("set_inode_field " + q(path) + " uid 0");
    cmds.push_back("set_inode_field " + q(path) + " gid 0"); cmds.push_back("ea_set -f " + q(ctxf) + " " + q(path) + " security.selinux");
}
static bool putApp(Project& P, const std::string& apk, bool priv) {
    std::string stem = baseName(apk); size_t dot = stem.rfind('.'); if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::string name; for (char ch : stem) if (isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '-') name += ch; if (name.empty()) name = "App";
    std::string base = P.prefix + (priv ? "/priv-app" : "/app"), d = base + "/" + name;
    if (existsIn(P.img, d)) { err(d + " already exists"); return false; }
    std::string tmpd = makeTempDir(), ctxf = tmpd + "/ctx.bin"; { Bytes b(CTX, CTX + sizeof CTX); writeAll(ctxf, b); }   // includes the trailing NUL
    Manifest man = parseManifest(apk); std::vector<std::string> cmds = {"mkdir " + q(d), "write " + q(apk) + " " + q(d + "/" + name + ".apk")};
    setMeta(cmds, d, "040755", ctxf); setMeta(cmds, d + "/" + name + ".apk", "0100644", ctxf);
    Zip z; if (z.open(apk)) {
        std::vector<std::string> libs; for (auto& n : z.names()) if (startsWith(n, "lib/") && n.size() > 3 && n.substr(n.size() - 3) == ".so") libs.push_back(n);
        if (!libs.empty()) {
            std::map<std::string, std::string> amap = {{"arm64-v8a", "arm64"}, {"armeabi-v7a", "arm"}, {"x86_64", "x86_64"}, {"x86", "x86"}}; std::vector<std::string> present;
            for (std::string a : {"arm64-v8a", "armeabi-v7a"}) for (auto& n : libs) if (startsWith(n, "lib/" + a + "/")) { present.push_back(a); break; }
            cmds.push_back("mkdir " + q(d + "/lib")); setMeta(cmds, d + "/lib", "040755", ctxf);
            for (auto& a : present) { std::string ld = d + "/lib/" + amap[a]; cmds.push_back("mkdir " + q(ld)); setMeta(cmds, ld, "040755", ctxf);
                for (auto& n : libs) if (startsWith(n, "lib/" + a + "/") && std::count(n.begin(), n.end(), '/') == 2) {
                    Bytes b; if (!z.read(n, b)) continue; std::string lp = tmpd + "/" + a + "_" + baseName(n); writeAll(lp, b);
                    cmds.push_back("write " + q(lp) + " " + q(ld + "/" + baseName(n))); setMeta(cmds, ld + "/" + baseName(n), "0100644", ctxf); } }
            say("  native libs extracted for " + (present.empty() ? std::string("none of arm64-v8a, armeabi-v7a") : join(present, ", ")), DM);
        }
    }
    if (priv) {
        if (!man.ok) { err("cannot read the manifest, so no privapp-permissions whitelist can be generated -> install to /app instead"); removeAll(tmpd); return false; }
        std::set<std::string> perms(man.perms.begin(), man.perms.end());
        std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<permissions>\n    <privapp-permissions package=\"" + man.package + "\">\n";
        for (auto& p : perms) xml += "        <permission name=\"" + p + "\"/>\n"; xml += "    </privapp-permissions>\n</permissions>\n";
        std::string xp = tmpd + "/perm.xml", rp = P.prefix + "/etc/permissions/privapp-permissions-" + name + ".xml"; writeText(xp, xml);
        cmds.push_back("write " + q(xp) + " " + q(rp)); setMeta(cmds, rp, "0100644", ctxf);
    }
    auto r = dbg(P.img, cmds, true); removeAll(tmpd);
    if (!existsIn(P.img, d + "/" + name + ".apk")) { err("write failed: " + r.err.substr(r.err.size() > 300 ? r.err.size() - 300 : 0)); return false; }
    P.added.push_back(d); P.save(); ok("added " + d + "  (package " + (man.ok ? man.package : "unknown") + (priv ? ", privileged" : "") + ")"); return true;
}
static void actAdd(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    say("put your .apk files in " + P.root + "/apps  or give paths separated by ';'", DM);
    std::string r = ask("apk paths (empty = everything in apps/)"); std::vector<std::string> files;
    if (!r.empty()) { for (auto& x : splitStr(r, ';')) { std::string s = x; size_t a = s.find_first_not_of(" \""), b = s.find_last_not_of(" \""); if (a != std::string::npos) files.push_back(s.substr(a, b - a + 1)); } }
    else { auto o = run({"ls", "-1", P.root + "/apps"}); for (auto& f : splitStr(o.out, '\n')) { std::string l = f; std::transform(l.begin(), l.end(), l.begin(), ::tolower);
        if (l.size() > 4 && l.substr(l.size() - 4) == ".apk") files.push_back(P.root + "/apps/" + f); } }
    if (files.empty()) { warn("no apk files"); return; }
    bool priv = ask("install as privileged system app (priv-app)? y/N", "n")[0] == 'y';
    for (auto& f : files) { if (!fileExists(f)) { err("missing " + f); continue; } putApp(P, f, priv); }
}
static bool setProp(Project& P, const std::string& k, const std::string& v) {
    std::string path = P.prefix + "/build.prop"; auto b = readFileIn(P.img, path); std::string txt(b.begin(), b.end());
    if (txt.empty()) { err("cannot read " + path); return false; }
    std::stringstream ss(txt); std::string l, out; bool done = false;
    while (std::getline(ss, l)) { if (startsWith(l, k + "=")) { out += k + "=" + v + "\n"; done = true; } else out += l + "\n"; }
    if (!done) out += k + "=" + v + "\n";
    std::string tmpd = makeTempDir(), tmp = tmpd + "/build.prop", ctxf = tmpd + "/ctx.bin"; writeText(tmp, out); writeAll(ctxf, Bytes(CTX, CTX + sizeof CTX));
    std::vector<std::string> cmds = {"rm " + q(path), "write " + q(tmp) + " " + q(path)}; setMeta(cmds, path, "0100644", ctxf);
    dbg(P.img, cmds, true); removeAll(tmpd); ok(k + "=" + v + " written"); return true;
}
static bool bumpFp(Project& P) {
    auto b = readFileIn(P.img, P.prefix + "/build.prop"); std::stringstream ss(std::string(b.begin(), b.end())); std::string l, cur;
    while (std::getline(ss, l)) if (startsWith(l, "ro.build.fingerprint=")) cur = l.substr(21);
    if (cur.empty()) { err("no fingerprint in build.prop"); return false; }
    size_t m = cur.rfind("-mux"); if (m != std::string::npos) cur = cur.substr(0, m);
    std::string v = cur + "-mux" + std::to_string(P.added.size() + 1); say("new fingerprint: " + v, AM); return setProp(P, "ro.build.fingerprint", v);
}
static void actProp(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    std::string k = ask("property to set (e.g. ro.product.model) or 'fp' to bump the fingerprint"); if (k.empty()) return;
    if (k == "fp") bumpFp(P); else setProp(P, k, ask("value"));
}
static void actFlash() {
    box("FLASH (fastboot, bootloader unlocked)", {c(AM, "1) verification off (pick one):"),
        "   fastboot --disable-verity --disable-verification flash vbmeta google-vbmeta.img   (proven on this tablet by GSI guides)",
        "   fastboot --disable-verity --disable-verification flash vbmeta stock-vbmeta.img    (plan B)",
        c(AM, "2) system:"), "   fastboot flash system system_custom.img", c(AM, "3) first boot:"),
        "   factory reset (or bump the fingerprint with menu 5) so the new system apps get their default permissions",
        c(DM, "keep stock system.img / vbmeta.img / boot.img safe: Lenovo Rescue can restore them")});
}
static void actRepack(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    say("checking filesystem ...", DM); run({"e2fsck", "-fy", P.img});
    auto r = run({"resize2fs", "-f", P.img, std::to_string(P.orig_blocks)});
    if (r.rc != 0) { err("cannot shrink back to the original size - too much data. Delete more apps.\n" + r.err); return; }
    if (truncate(P.img.c_str(), (off_t)P.orig_bytes) != 0) { err("truncate failed"); return; }
    auto f = run({"e2fsck", "-fn", P.img}); if (f.rc != 0) warn("e2fsck reports problems:\n" + f.out.substr(f.out.size() > 400 ? f.out.size() - 400 : 0));
    std::string raw = P.root + "/out/system_custom.raw.img", sp = P.root + "/out/system_custom.img";
    copyFile(P.img, raw); say("making sparse image ...", DM); if (!toSparse(raw, sp)) { err("sparse conversion failed"); return; }
    auto info = fsInfo(raw); auto sha = run({"sha256sum", sp}); std::string h = sha.out.substr(0, sha.out.find(' '));
    box("REPACK DONE", {"raw    : " + raw + "  (" + human(fileSize(raw)) + ", same size as stock)", "sparse : " + sp + "  (" + human(fileSize(sp)) + ")",
                        "free space left in system: " + human(info.free * info.bs), "sha256 : " + h}, GR);
    actFlash();
}

// ------------------------------------------------------------------ non-interactive commands (GUI)
static std::string tabs(const std::vector<std::string>& v) { return join(v, ";"); }
static std::string oneLine(std::string s) { for (auto& ch : s) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' '; return s; }
static void cmdStatus(Project& P) {
    auto line = [](const std::string& k, const std::string& v) { std::cout << "#STATUS\t" << k << "\t" << oneLine(v) << "\n"; };
    line("project", P.root); line("image loaded", P.have() ? "yes" : "no");
    if (!P.have()) return;
    auto i = fsInfo(P.img);
    line("original size", human(P.orig_bytes)); line("working fs", human(i.blocks * i.bs) + ", free " + human(i.free * i.bs));
    line("features", i.features); line("app root", P.prefix.empty() ? "/" : P.prefix);
    line("apps removed", std::to_string(P.removed.size())); line("apps added", std::to_string(P.added.size()));
    auto bp = readFileIn(P.img, P.prefix + "/build.prop"); std::stringstream ss(std::string(bp.begin(), bp.end())); std::string l;
    while (std::getline(ss, l)) for (auto k : {"ro.build.display.id", "ro.build.version.release", "ro.build.fingerprint", "ro.product.model", "ro.product.name", "ro.product.device", "ro.product.brand"})
        if (startsWith(l, std::string(k) + "=")) line(k, l.substr(strlen(k) + 1));
}
static void cmdDoctor() {
    for (auto t : {"debugfs", "e2fsck", "resize2fs", "dumpe2fs", "sha256sum"}) std::cout << "#TOOL\t" << t << "\t" << (haveTool(t) ? "ok" : "MISSING") << "\n";
    say("tools: sudo apt install -y e2fsprogs coreutils", DM);
}
static void cmdList(Project& P) {
    if (!P.have()) { err("unpack first"); return; }
    for (auto& it : scanApps(P))
        std::cout << "#APP\t" << it.path << "\t" << it.name << "\t" << it.pkg << "\t" << it.size << "\t" << (it.risky ? 1 : 0) << "\t" << it.shared << "\t"
                  << tabs(it.needs) << "\t" << tabs(it.neededby) << "\t" << tabs(it.mates) << "\t" << oneLine(it.why) << "\n";
}
static void cmdDelete(Project& P, const std::vector<std::string>& paths) {
    if (!P.have()) { err("unpack first"); return; }
    std::vector<std::string> cmds;
    for (auto& p : paths) {
        size_t sl = p.find_last_of('/'); std::string par = sl == 0 ? "/" : p.substr(0, sl), nm = p.substr(sl + 1); char kind = 0;
        auto lm = lsDir(P.img, {par}); for (auto& e : lm[par]) if (e.name == nm) kind = e.kind;
        if (!kind) { err("not found: " + p); continue; }
        if (kind == 'd') { auto t = walk(P.img, {p})[p]; for (auto& f : t.files) cmds.push_back("rm " + q(f.first));
            std::sort(t.dirs.begin(), t.dirs.end(), [](const std::string& a, const std::string& b) { return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/'); });
            for (auto& d : t.dirs) cmds.push_back("rmdir " + q(d)); }
        else cmds.push_back("rm " + q(p));
    }
    if (cmds.empty()) return;
    dbg(P.img, cmds, true);
    std::string log; for (auto& p : paths) { if (existsIn(P.img, p)) err("could not delete " + p); else { ok("deleted " + p); P.removed.push_back(p); } }
    P.save(); for (auto& x : P.removed) log += x + "\n"; writeText(P.root + "/out/removed_apps.txt", log);
}
static void cmdExport(Project& P, const std::string& dest) {
    system(("mkdir -p '" + dest + "'").c_str()); bool any = false;
    for (auto n : {"system_custom.img", "system_custom.raw.img"}) { std::string src = P.root + "/out/" + n; if (fileExists(src)) { if (copyFile(src, dest + "/" + n)) { ok("exported " + dest + "/" + n); any = true; } else err("copy failed: " + src); } }
    if (!any) err("nothing to export - repack first");
}

static std::string resolveProject(const std::string& a) {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    if (a.empty()) return home + "/system-kitchen/project1";
    if (a[0] == '~') return home + a.substr(1);
    if (a.find('/') != std::string::npos) return a;
    return home + "/system-kitchen/" + a;
}
static void menuLoop(Project& P, const std::vector<std::string>& miss) {
    while (true) {
        banner();
        std::vector<std::string> pl = {P.root, std::string("state: ") + (P.have() ? "image loaded" : "empty - choose 1")};
        if (!miss.empty()) pl.push_back(c(RD, "missing tools: " + join(miss, ", ") + "  ->  sudo apt install e2fsprogs"));
        box("PROJECT", pl, DM);
        const char* menu[][2] = {{"1", "Unpack stock system.img"}, {"2", "Image info"}, {"3", "Apps: list / dependencies / delete"}, {"4", "Add your apps (MUX apps)"},
                                 {"5", "Edit build.prop (model, name, fingerprint bump)"}, {"6", "Repack -> system_custom.img"}, {"7", "Flash guide"}, {"0", "Quit"}};
        for (auto& m : menu) std::cout << c(AM, std::string(" ") + m[0] + " ") << c(CY, m[1]) << "\n";
        std::string ch = ask("select");
        if (ch == "1") actUnpack(P); else if (ch == "2") actInfo(P); else if (ch == "3") actDelete(P); else if (ch == "4") actAdd(P);
        else if (ch == "5") actProp(P); else if (ch == "6") actRepack(P); else if (ch == "7") actFlash(); else if (ch == "0" || (ch.empty() && !std::cin)) return;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    std::string projArg; std::vector<std::string> rest;
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--project" && i + 1 < argc) projArg = argv[++i]; else rest.push_back(a); }
    std::string root = resolveProject(projArg);
    std::vector<std::string> miss; for (auto t : {"debugfs", "e2fsck", "resize2fs", "dumpe2fs"}) if (!haveTool(t)) miss.push_back(t);
    if (system(("mkdir -p '" + root + "'").c_str()) != 0) {}
    Project P(root);
    if (rest.empty()) { menuLoop(P, miss); return 0; }
    g_cli = true; const std::string cmd = rest[0]; std::vector<std::string> a(rest.begin() + 1, rest.end());
    if (cmd == "doctor") cmdDoctor();
    else if (!miss.empty() && cmd != "status") { err("missing tools: " + join(miss, ", ") + "  ->  sudo apt install -y e2fsprogs"); }
    else if (cmd == "status") cmdStatus(P);
    else if (cmd == "unpack") { if (a.empty()) err("usage: unpack <system.img> [extraMB]"); else actUnpack(P, a[0], a.size() > 1 ? atoi(a[1].c_str()) : -1); }
    else if (cmd == "list") cmdList(P);
    else if (cmd == "delete") cmdDelete(P, a);
    else if (cmd == "add") { bool priv = false; std::vector<std::string> files; for (auto& x : a) { if (x == "--priv") priv = true; else files.push_back(x); }
        if (!P.have()) err("unpack first"); else for (auto& f : files) { if (!fileExists(f)) err("missing " + f); else putApp(P, f, priv); } }
    else if (cmd == "prop") { if (!P.have()) err("unpack first"); else if (a.size() == 1 && a[0] == "fp") bumpFp(P); else if (a.size() >= 2) setProp(P, a[0], a[1]); else err("usage: prop <key> <value> | prop fp"); }
    else if (cmd == "repack") actRepack(P);
    else if (cmd == "export") { if (a.empty()) err("usage: export <dest folder>"); else cmdExport(P, a[0]); }
    else err("unknown command: " + cmd);
    return g_failed ? 1 : 0;
}