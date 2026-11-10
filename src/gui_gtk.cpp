// MUX // SYSTEM KITCHEN - GTK3 GUI for WSL (WSLg) / Linux.
// The ext4 work is done by the "kitchen" backend binary that sits next to this program; the GUI runs it as a child process.
#include <gtk/gtk.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

enum Kind { K_STATUS, K_DOCTOR, K_UNPACK, K_LIST, K_DELETE, K_ADD, K_PROP, K_REPACK, K_EXPORT };
struct Job { int kind; std::vector<std::string> args; };
struct Row {
    std::string path, name, pkg, shared, why; unsigned long long size = 0; bool risky = false, checked = false;
    std::vector<std::string> needs, neededBy, mates;
};
enum { C_CHK, C_NAME, C_PKG, C_SIZE, C_CLASS, C_DEPS, C_IDX, C_COLOR, C_N };

static GtkWidget *gWin, *gHeadState, *gProj, *gImg, *gGrow, *gStatusView, *gSearch, *gAppView, *gDetail, *gAppStatus, *gAddView, *gPriv,
                 *gKey, *gVal, *gLog;
static GtkListStore *gAppStore, *gAddStore;
static std::vector<GtkWidget*> gActionBtns, gFilterBtns;
static std::vector<Row> gRows;
static std::vector<int> gView;
static std::vector<std::string> gAddFiles;
static std::vector<Job> gQueue;
static std::atomic<bool> gBusy{false};
static GtkWidget* gProg; static guint gTimer = 0; static gint64 gT0 = 0; static std::string gJobName;
static gboolean tick(gpointer) { if (!gBusy) return G_SOURCE_CONTINUE; gtk_progress_bar_pulse(GTK_PROGRESS_BAR(gProg)); int sec = (int)((g_get_monotonic_time() - gT0) / 1000000);
    char b[200]; snprintf(b, sizeof b, "WORKING: %s  [%d:%02d]  - big images take minutes, log updates as steps finish", gJobName.c_str(), sec / 60, sec % 60); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(gProg), b); return G_SOURCE_CONTINUE; }
static bool gFilling = false;
static int gFilter = 0;   // 0 all, 1 safe, 2 risky

// ---------------------------------------------------------------- helpers
static std::string selfDir() {
    char b[4096]; ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1); if (n <= 0) return "."; b[n] = 0;
    std::string s = b; size_t p = s.find_last_of('/'); return p == std::string::npos ? "." : s.substr(0, p);
}
static std::vector<std::string> splitC(const std::string& s, char d) {
    std::vector<std::string> v; std::string cur; for (char c : s) { if (c == d) { v.push_back(cur); cur.clear(); } else cur += c; } v.push_back(cur); return v;
}
static std::vector<std::string> splitSemi(const std::string& s) { std::vector<std::string> r; for (auto& x : splitC(s, ';')) if (!x.empty()) r.push_back(x); return r; }
static std::string joinS(const std::vector<std::string>& v, size_t cap = 12) {
    std::string s; for (size_t i = 0; i < v.size() && i < cap; i++) { if (i) s += ", "; s += v[i]; }
    if (v.size() > cap) s += ", +" + std::to_string(v.size() - cap) + " more"; return s;
}
static std::string humanSize(unsigned long long n) {
    const char* u[] = {"B", "KB", "MB", "GB"}; double d = (double)n; int i = 0; while (d >= 1024 && i < 3) { d /= 1024; i++; }
    char b[64]; if (i == 0) snprintf(b, sizeof b, "%llu B", n); else snprintf(b, sizeof b, "%.1f %s", d, u[i]); return b;
}
static std::string entryText(GtkWidget* e) {
    std::string s = gtk_entry_get_text(GTK_ENTRY(e)); size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static void postUi(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    g_idle_add([](gpointer d) -> gboolean { auto* fn = (std::function<void()>*)d; (*fn)(); delete fn; return G_SOURCE_REMOVE; }, p);
}
static void setText(GtkWidget* tv, const std::string& s) { gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), s.c_str(), -1); }
static void appendLog(const std::string& s) {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gLog)); GtkTextIter end; gtk_text_buffer_get_end_iter(b, &end);
    std::string t = s + "\n"; gtk_text_buffer_insert(b, &end, t.c_str(), -1); gtk_text_buffer_get_end_iter(b, &end);
    GtkTextMark* m = gtk_text_buffer_get_mark(b, "end"); if (!m) m = gtk_text_buffer_create_mark(b, "end", &end, FALSE); else gtk_text_buffer_move_mark(b, m, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gLog), m);
}
static bool confirm(const std::string& msg, bool warn = false) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(gWin), GTK_DIALOG_MODAL, warn ? GTK_MESSAGE_WARNING : GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg.c_str());
    int r = gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return r == GTK_RESPONSE_YES;
}
static void info(const std::string& msg) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(gWin), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}
static void addClass(GtkWidget* w, const char* c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }
static void delClass(GtkWidget* w, const char* c) { gtk_style_context_remove_class(gtk_widget_get_style_context(w), c); }

// ---------------------------------------------------------------- backend runner
static void jobDone(int kind, int rc, std::vector<Row> rows, std::string status) {
    if (kind == K_LIST && rc == 0) {
        std::set<std::string> keep; for (auto& r : gRows) if (r.checked) keep.insert(r.path);
        gRows = rows; for (auto& r : gRows) if (keep.count(r.path)) r.checked = true;
        std::sort(gRows.begin(), gRows.end(), [](const Row& a, const Row& b) { return a.size > b.size; });
        extern void fillApps(); fillApps(); appendLog("Loaded " + std::to_string(gRows.size()) + " apps.");
    }
    if (!status.empty()) {
        setText(gStatusView, status);
        if (kind == K_DOCTOR) gtk_label_set_text(GTK_LABEL(gHeadState), status.find("MISSING") != std::string::npos ? "BACKEND: tools MISSING (sudo apt install e2fsprogs)" : "BACKEND: ready");
    }
    if (kind == K_DOCTOR && rc != 0) gtk_label_set_text(GTK_LABEL(gHeadState), "BACKEND: kitchen not found next to kitchen-gui");
    gBusy = false; for (auto* b : gActionBtns) gtk_widget_set_sensitive(b, TRUE);
    appendLog(rc == 0 ? "[DONE] " + gJobName : "[FAILED] " + gJobName);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gProg), rc == 0 ? 1.0 : 0.0); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(gProg), rc == 0 ? "IDLE - last job finished OK" : "IDLE - last job FAILED, see log");
    if (rc == 0 && !gQueue.empty()) { Job j = gQueue.front(); gQueue.erase(gQueue.begin()); extern void startJob(int, std::vector<std::string>); startJob(j.kind, j.args); }
    else gQueue.clear();
}
static void worker(Job j, std::string proj) {
    std::vector<std::string> av = {selfDir() + "/kitchen", "--project", proj}; for (auto& a : j.args) av.push_back(a);
    int po[2]; if (pipe(po) != 0) { postUi([=] { appendLog("[ERROR] pipe failed"); jobDone(j.kind, 1, {}, ""); }); return; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(po[1], 1); dup2(po[1], 2); close(po[0]); close(po[1]);
        std::vector<char*> a; for (auto& s : av) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
        execv(a[0], a.data()); _exit(127);
    }
    close(po[1]); std::vector<Row> rows; std::string status, acc; char buf[8192]; ssize_t n;
    auto handle = [&](std::string line) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#APP\t", 0) == 0) {
            auto f = splitC(line, '\t'); if (f.size() < 11) return;
            Row r; r.path = f[1]; r.name = f[2]; r.pkg = f[3]; r.size = strtoull(f[4].c_str(), nullptr, 10); r.risky = f[5] == "1"; r.shared = f[6];
            r.needs = splitSemi(f[7]); r.neededBy = splitSemi(f[8]); r.mates = splitSemi(f[9]); r.why = f[10]; rows.push_back(r);
        } else if (line.rfind("#STATUS\t", 0) == 0 || line.rfind("#TOOL\t", 0) == 0) {
            auto f = splitC(line, '\t'); if (f.size() >= 3) { std::string k = f[1]; for (auto& ch : k) ch = (char)toupper((unsigned char)ch); status += k + " : " + f[2] + "\n"; }
        } else if (!line.empty()) postUi([line] { appendLog(line); });
    };
    while ((n = read(po[0], buf, sizeof buf)) > 0) { acc.append(buf, (size_t)n); size_t p; while ((p = acc.find('\n')) != std::string::npos) { handle(acc.substr(0, p)); acc.erase(0, p + 1); } }
    if (!acc.empty()) handle(acc);
    close(po[0]); int st = 0; waitpid(pid, &st, 0); int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    postUi([=] { if (rc == 127) appendLog("[ERROR] backend 'kitchen' not found in " + selfDir() + " - run 'make' first"); else if (rc != 0) appendLog("[FAIL] backend exit code " + std::to_string(rc)); jobDone(j.kind, rc, rows, status); });
}
void startJob(int kind, std::vector<std::string> args) {
    if (gBusy) return;
    std::string proj = entryText(gProj); if (proj.empty()) proj = "project1";
    gBusy = true; for (auto* b : gActionBtns) gtk_widget_set_sensitive(b, FALSE);
    gJobName = args.empty() ? "job" : args[0]; gT0 = g_get_monotonic_time(); appendLog("[RUN] kitchen " + joinS(args, 4)); tick(nullptr);
    std::thread(worker, Job{kind, args}, proj).detach();
}
static void startQueue(std::vector<Job> q) { if (q.empty() || gBusy) return; gQueue.assign(q.begin() + 1, q.end()); startJob(q[0].kind, q[0].args); }

// ---------------------------------------------------------------- apps tab
static void showDetail(int idx) {
    if (idx < 0 || idx >= (int)gRows.size()) { setText(gDetail, ""); return; }
    const Row& r = gRows[idx]; std::string s = r.name + "   (" + (r.pkg.empty() ? "?" : r.pkg) + ")\n" + r.path + "\n";
    s += std::string("CLASS: ") + (r.risky ? "RISKY" : "SAFE") + "   SIZE: " + humanSize(r.size) + "\n";
    if (!r.why.empty()) s += "NOTE: " + r.why + "\n";
    if (!r.shared.empty()) s += "SHARED UID: " + r.shared + (r.mates.empty() ? "" : "   shared with: " + joinS(r.mates)) + "\n";
    s += "NEEDS: " + (r.needs.empty() ? std::string("-") : joinS(r.needs)) + "\n";
    s += "NEEDED BY: " + (r.neededBy.empty() ? std::string("-") : joinS(r.neededBy)) + "\n";
    setText(gDetail, s);
}
static void updateAppStatus() {
    int t = 0, risky = 0; unsigned long long sz = 0; for (auto& r : gRows) { if (r.checked) { t++; sz += r.size; } if (r.risky) risky++; }
    std::string s = std::to_string(t) + " ticked (" + humanSize(sz) + ")   |   " + std::to_string(gView.size()) + " shown / " + std::to_string(gRows.size()) + "   |   " + std::to_string(risky) + " risky";
    gtk_label_set_text(GTK_LABEL(gAppStatus), s.c_str());
}
void fillApps() {
    gFilling = true; gtk_list_store_clear(gAppStore);
    std::string f = entryText(gSearch); std::transform(f.begin(), f.end(), f.begin(), ::tolower); gView.clear();
    for (size_t i = 0; i < gRows.size(); i++) {
        auto& r = gRows[i];
        if (gFilter == 1 && r.risky) continue; if (gFilter == 2 && !r.risky) continue;
        if (!f.empty()) { std::string l = r.name + " " + r.pkg; std::transform(l.begin(), l.end(), l.begin(), ::tolower); if (l.find(f) == std::string::npos) continue; }
        gView.push_back((int)i);
        GtkTreeIter it; gtk_list_store_append(gAppStore, &it);
        std::string d = std::to_string(r.needs.size()) + " / " + std::to_string(r.neededBy.size()), sz = humanSize(r.size);
        gtk_list_store_set(gAppStore, &it, C_CHK, r.checked ? TRUE : FALSE, C_NAME, r.name.c_str(), C_PKG, r.pkg.empty() ? "?" : r.pkg.c_str(), C_SIZE, sz.c_str(),
                           C_CLASS, r.risky ? "RISKY" : "SAFE", C_DEPS, d.c_str(), C_IDX, (int)i, C_COLOR, r.risky ? "#ff3d57" : "#00e5ff", -1);
    }
    gFilling = false; showDetail(-1); updateAppStatus();
}
static void onToggle(GtkCellRendererToggle*, gchar* path, gpointer) {
    GtkTreeIter it; if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(gAppStore), &it, path)) return;
    int idx = 0; gtk_tree_model_get(GTK_TREE_MODEL(gAppStore), &it, C_IDX, &idx, -1);
    if (idx < 0 || idx >= (int)gRows.size()) return;
    gRows[idx].checked = !gRows[idx].checked; gtk_list_store_set(gAppStore, &it, C_CHK, gRows[idx].checked ? TRUE : FALSE, -1); updateAppStatus();
}
static void onAppSel(GtkTreeSelection* sel, gpointer) {
    if (gFilling) return; GtkTreeModel* m; GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) { int idx = -1; gtk_tree_model_get(m, &it, C_IDX, &idx, -1); showDetail(idx); }
}
static void setFilter(int f) {
    gFilter = f; for (int i = 0; i < 3; i++) { if (i == f) addClass(gFilterBtns[i], "on"); else delClass(gFilterBtns[i], "on"); } fillApps();
}
static void onDelete(GtkWidget*, gpointer) {
    if (gBusy) return;
    std::set<std::string> ticked; std::vector<std::string> paths; unsigned long long tot = 0;
    for (auto& r : gRows) if (r.checked) { ticked.insert(r.name); paths.push_back(r.path); tot += r.size; }
    if (paths.empty()) { info("Tick some apps first."); return; }
    std::string risky, deps; int nr = 0, nd = 0;
    for (auto& r : gRows) if (r.checked) {
        if (r.risky) { nr++; if (nr <= 8) risky += "\n  " + r.name; }
        std::vector<std::string> bl; for (auto& n : r.neededBy) if (!ticked.count(n)) bl.push_back(n);
        if (!bl.empty()) { nd++; if (nd <= 6) deps += "\n  " + r.name + "  <-  needed by " + joinS(bl, 3); }
    }
    std::string m;
    if (nr) m += std::to_string(nr) + " RISKY app(s) selected (can bootloop or break calls / keyboard / Play services):" + risky + "\n\n";
    if (nd) m += std::to_string(nd) + " app(s) are still needed by other apps that stay installed:" + deps + "\n\n";
    m += "Delete " + std::to_string(paths.size()) + " app(s), " + humanSize(tot) + ", from the working image?";
    if (!confirm(m, nr || nd)) return;
    std::vector<std::string> a = {"delete"}; a.insert(a.end(), paths.begin(), paths.end());
    startQueue({Job{K_DELETE, a}, Job{K_LIST, {"list"}}, Job{K_STATUS, {"status"}}});
}

// ---------------------------------------------------------------- other tabs
static std::string chooseFile(const char* title, const char* pattern, bool folder = false) {
    GtkWidget* d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(gWin), folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (pattern) { GtkFileFilter* f = gtk_file_filter_new(); gtk_file_filter_set_name(f, pattern); gtk_file_filter_add_pattern(f, pattern); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
        GtkFileFilter* all = gtk_file_filter_new(); gtk_file_filter_set_name(all, "All files"); gtk_file_filter_add_pattern(all, "*"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all); }
    std::string r; if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) { char* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)); if (f) { r = f; g_free(f); } }
    gtk_widget_destroy(d); return r;
}
static void onBrowse(GtkWidget*, gpointer) { std::string f = chooseFile("Select the stock system.img", "*.img"); if (!f.empty()) gtk_entry_set_text(GTK_ENTRY(gImg), f.c_str()); }
static void onUnpack(GtkWidget*, gpointer) {
    std::string img = entryText(gImg); if (img.empty()) { info("Choose the stock system.img first."); return; }
    if (!confirm("Unpack replaces the working image of this project (the stock file itself is not touched).\nA 4 GB image can take several minutes. Continue?")) return;
    std::vector<std::string> a = {"unpack", img}; std::string g = entryText(gGrow); if (!g.empty()) a.push_back(g);
    startQueue({Job{K_UNPACK, a}, Job{K_STATUS, {"status"}}, Job{K_LIST, {"list"}}});
}
static void fillAdd() {
    gtk_list_store_clear(gAddStore);
    for (auto& f : gAddFiles) { GtkTreeIter it; gtk_list_store_append(gAddStore, &it); FILE* fp = fopen(f.c_str(), "rb"); std::string sz = "?";
        if (fp) { fseek(fp, 0, SEEK_END); sz = humanSize((unsigned long long)ftell(fp)); fclose(fp); } gtk_list_store_set(gAddStore, &it, 0, f.c_str(), 1, sz.c_str(), -1); }
}
static void addApk(const std::string& f) {
    std::string l = f; std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if (l.size() < 5 || l.substr(l.size() - 4) != ".apk") return; if (std::find(gAddFiles.begin(), gAddFiles.end(), f) == gAddFiles.end()) gAddFiles.push_back(f);
}
static void onAddFiles(GtkWidget*, gpointer) {
    GtkWidget* d = gtk_file_chooser_dialog_new("Select your APK files", GTK_WINDOW(gWin), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d), TRUE);
    GtkFileFilter* f = gtk_file_filter_new(); gtk_file_filter_set_name(f, "APK files"); gtk_file_filter_add_pattern(f, "*.apk"); gtk_file_filter_add_pattern(f, "*.APK"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        GSList* l = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d)); for (GSList* p = l; p; p = p->next) addApk((char*)p->data); g_slist_free_full(l, g_free); fillAdd();
    }
    gtk_widget_destroy(d);
}
static void onAddRemove(GtkWidget*, gpointer) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(gAddView)); GtkTreeModel* m; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return; char* f = nullptr; gtk_tree_model_get(m, &it, 0, &f, -1);
    if (f) { gAddFiles.erase(std::remove(gAddFiles.begin(), gAddFiles.end(), std::string(f)), gAddFiles.end()); g_free(f); } fillAdd();
}
static void onAddClear(GtkWidget*, gpointer) { gAddFiles.clear(); fillAdd(); }
static void onInstall(GtkWidget*, gpointer) {
    if (gAddFiles.empty()) { info("Add .apk files first."); return; }
    bool priv = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gPriv));
    if (!confirm("Install " + std::to_string(gAddFiles.size()) + " app(s) into the working image as " + (priv ? "PRIVILEGED apps (priv-app + permission whitelist)?" : "normal system apps (/system/app)?"))) return;
    std::vector<std::string> a = {"add"}; if (priv) a.push_back("--priv"); for (auto& f : gAddFiles) a.push_back(f);
    startQueue({Job{K_ADD, a}, Job{K_LIST, {"list"}}, Job{K_STATUS, {"status"}}});
}
static void onSetProp(GtkWidget*, gpointer) {
    std::string k = entryText(gKey), v = entryText(gVal); if (k.empty()) { info("Enter a property name (for example ro.product.model)."); return; }
    startQueue({Job{K_PROP, {"prop", k, v}}, Job{K_STATUS, {"status"}}});
}
static void onBump(GtkWidget*, gpointer) { startQueue({Job{K_PROP, {"prop", "fp"}}, Job{K_STATUS, {"status"}}}); }
static void onRepack(GtkWidget*, gpointer) {
    if (!confirm("Repack shrinks the filesystem back to the ORIGINAL size and writes system_custom.img.\nContinue?")) return;
    startQueue({Job{K_REPACK, {"repack"}}, Job{K_STATUS, {"status"}}});
}
static void onExport(GtkWidget*, gpointer) { std::string d = chooseFile("Choose a folder for the finished images", nullptr, true); if (!d.empty()) startJob(K_EXPORT, {"export", d}); }
static void onDoctor(GtkWidget*, gpointer) { startJob(K_DOCTOR, {"doctor"}); }
static void onStatus(GtkWidget*, gpointer) { startJob(K_STATUS, {"status"}); }
static void onLoad(GtkWidget*, gpointer) { startQueue({Job{K_LIST, {"list"}}, Job{K_STATUS, {"status"}}}); }
static void onClear(GtkWidget*, gpointer) { for (auto& r : gRows) r.checked = false; fillApps(); }
static void onSearch(GtkEditable*, gpointer) { fillApps(); }
static void onQuick(GtkWidget* b, gpointer) { gtk_entry_set_text(GTK_ENTRY(gKey), gtk_button_get_label(GTK_BUTTON(b))); }
static void onDrop(GtkWidget*, GdkDragContext*, gint, gint, GtkSelectionData* sd, guint, guint, gpointer) {
    gchar** uris = gtk_selection_data_get_uris(sd); if (!uris) return; std::vector<std::string> files;
    for (int i = 0; uris[i]; i++) { gchar* f = g_filename_from_uri(uris[i], nullptr, nullptr); if (f) { files.push_back(f); g_free(f); } }
    g_strfreev(uris);
    if (files.size() == 1 && files[0].size() > 4 && files[0].substr(files[0].size() - 4) == ".img") gtk_entry_set_text(GTK_ENTRY(gImg), files[0].c_str());
    else { for (auto& f : files) addApk(f); fillAdd(); }
}

// ---------------------------------------------------------------- building the window
static GtkWidget* btn(const char* text, GCallback cb, const char* cls = nullptr, bool action = true) {
    GtkWidget* b = gtk_button_new_with_label(text); if (cb) g_signal_connect(b, "clicked", cb, nullptr); if (cls) addClass(b, cls);
    if (action) gActionBtns.push_back(b); return b;
}
static GtkWidget* lbl(const char* t) { GtkWidget* l = gtk_label_new(t); addClass(l, "lbl"); gtk_widget_set_halign(l, GTK_ALIGN_START); return l; }
static GtkWidget* scrolled(GtkWidget* child, int minH = -1) {
    GtkWidget* s = gtk_scrolled_window_new(nullptr, nullptr); gtk_container_add(GTK_CONTAINER(s), child);
    if (minH > 0) gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(s), minH); return s;
}
static GtkWidget* textView(bool mono = true) {
    GtkWidget* t = gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(t), FALSE); gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(t), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t), GTK_WRAP_WORD_CHAR); if (mono) gtk_text_view_set_monospace(GTK_TEXT_VIEW(t), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(t), 8); gtk_text_view_set_top_margin(GTK_TEXT_VIEW(t), 6); return t;
}
static GtkWidget* panel(GtkWidget* child) { GtkWidget* f = gtk_frame_new(nullptr); addClass(f, "panel"); gtk_container_add(GTK_CONTAINER(f), child); return f; }
static GtkWidget* vbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, sp); }
static GtkWidget* hbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, sp); }
static void pack(GtkWidget* box, GtkWidget* w, bool expand = false) { gtk_box_pack_start(GTK_BOX(box), w, expand, expand, 0); }
static void addTextCol(GtkWidget* tv, const char* title, int col, bool expand, bool colored = false) {
    GtkCellRenderer* r = gtk_cell_renderer_text_new(); GtkTreeViewColumn* c = colored ? gtk_tree_view_column_new_with_attributes(title, r, "text", col, "foreground", C_COLOR, nullptr)
                                                                                       : gtk_tree_view_column_new_with_attributes(title, r, "text", col, nullptr);
    gtk_tree_view_column_set_expand(c, expand); gtk_tree_view_column_set_resizable(c, TRUE); gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
}

static const char* CSS =
"window, .bg { background-color:#040a10; color:#e2eef4; font-family:'DejaVu Sans Mono','Consolas',monospace; font-size:10pt; }\n"
"label { color:#e2eef4; } .lbl { color:#ffb300; font-weight:bold; } .dimlbl { color:#6b8794; }\n"
".title { color:#00e5ff; font-size:20pt; font-weight:bold; } .sub { color:#6b8794; }\n"
".panel { background-color:#09141e; border:1px solid #0e4350; border-radius:0; padding:10px; }\n"
"button { background-image:none; background-color:#040a10; color:#00e5ff; border:1px solid #00e5ff; border-radius:0; padding:5px 12px; font-weight:bold; box-shadow:none; text-shadow:none; }\n"
"button:hover { background-color:#0a3a46; } button:disabled { color:#6b8794; border-color:#6b8794; }\n"
"button.danger { color:#ff3d57; border-color:#ff3d57; } button.danger:hover { background-color:#3a1019; }\n"
"button.amber { color:#ffb300; border-color:#ffb300; } button.amber:hover { background-color:#3a2a05; }\n"
"button.dim { color:#6b8794; border-color:#6b8794; } button.on { background-color:#0a3a46; }\n"
"entry { background-image:none; background-color:#09141e; color:#00e5ff; border:1px solid #0e4350; border-radius:0; caret-color:#00e5ff; box-shadow:none; padding:4px 6px; }\n"
"textview, textview text { background-color:#09141e; color:#e2eef4; }\n"
"scrolledwindow { border:1px solid #0e4350; }\n"
"notebook > header { background-color:#040a10; border:none; } notebook > header > tabs > tab { background-color:#040a10; color:#00e5ff; border:1px solid #00e5ff; border-radius:0; padding:6px 18px; margin-right:6px; font-weight:bold; }\n"
"notebook > header > tabs > tab:checked { background-color:#0a3a46; color:#ffb300; border-color:#ffb300; } notebook > stack { background-color:transparent; }\n"
"treeview.view { background-color:#09141e; color:#e2eef4; } treeview.view:selected { background-color:#0a3a46; color:#ffffff; }\n"
"treeview.view header button { background-image:none; background-color:#0a1c27; color:#ffb300; border:none; border-bottom:1px solid #0e4350; padding:4px 8px; }\n"
"check { background-image:none; background-color:#040a10; border:1px solid #00e5ff; color:#ffb300; } check:checked { background-color:#0a3a46; }\n"
"checkbutton label { color:#6b8794; font-weight:bold; }\n"
"scrollbar slider { background-color:#0e4350; min-width:8px; min-height:8px; } scrollbar trough { background-color:#040a10; }\n"
"dialog, dialog .background, messagedialog, messagedialog .background, messagedialog box, dialog box, dialog headerbar, messagedialog headerbar, filechooser, filechooser box, placessidebar, placessidebar list, placessidebar row { background-color:#09141e; background-image:none; color:#e2eef4; }\n"
"dialog label, messagedialog label, messagedialog .message-area label, messagedialog .titlebar label, filechooser label { color:#e2eef4; }\n"
"dialog headerbar, messagedialog headerbar, headerbar { background-image:none; background-color:#040a10; color:#00e5ff; border-bottom:1px solid #00e5ff; box-shadow:none; }\n"
"headerbar label, headerbar .title { color:#00e5ff; font-weight:bold; }\n"
"dialog actionbar, dialog .dialog-action-area { background-color:#09141e; }\n"
"dialog entry, filechooser entry, dialog treeview.view, filechooser treeview.view, dialog list, dialog row { background-color:#09141e; color:#e2eef4; }\n"
"dialog row:selected, filechooser row:selected, dialog treeview.view:selected { background-color:#0a3a46; color:#ffffff; }\n"
"dialog scrolledwindow, dialog frame { border-color:#0e4350; }\n"
"popover, popover.background, menu, menuitem { background-color:#09141e; color:#e2eef4; border:1px solid #0e4350; }\n"
"menuitem:hover { background-color:#0a3a46; }\n"
"frame > border { border:none; }\n"
"progressbar trough { background-color:#09141e; border:1px solid #0e4350; border-radius:0; min-height:20px; } progressbar progress { background-color:#00e5ff; border-radius:0; min-height:20px; } progressbar text { color:#ffb300; font-weight:bold; }\n";

static GtkWidget* tabProject() {
    GtkWidget* g = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(g), 10); gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gProj = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(gProj), "project1"); gtk_entry_set_width_chars(GTK_ENTRY(gProj), 22);
    gImg = gtk_entry_new(); gtk_widget_set_hexpand(gImg, TRUE); gGrow = gtk_entry_new(); gtk_entry_set_width_chars(GTK_ENTRY(gGrow), 8);
    GtkWidget* hint = gtk_label_new("stored in ~/system-kitchen/NAME"); addClass(hint, "dimlbl");
    gtk_grid_attach(GTK_GRID(g), lbl("PROJECT NAME"), 0, 0, 1, 1); gtk_grid_attach(GTK_GRID(g), gProj, 1, 0, 1, 1); gtk_grid_attach(GTK_GRID(g), hint, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), lbl("STOCK system.img"), 0, 1, 1, 1); gtk_grid_attach(GTK_GRID(g), gImg, 1, 1, 1, 1); gtk_grid_attach(GTK_GRID(g), btn("BROWSE", G_CALLBACK(onBrowse), "amber", false), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), lbl("EXTRA MB (blank = auto)"), 0, 2, 1, 1); gtk_grid_attach(GTK_GRID(g), gGrow, 1, 2, 1, 1);
    GtkWidget* hb = hbox(); pack(hb, btn("UNPACK", G_CALLBACK(onUnpack), "danger")); pack(hb, btn("CHECK TOOLS", G_CALLBACK(onDoctor))); pack(hb, btn("REFRESH STATUS", G_CALLBACK(onStatus)));
    gtk_grid_attach(GTK_GRID(g), hb, 2, 2, 1, 1);
    gStatusView = textView(); GtkWidget* sw = scrolled(gStatusView); gtk_widget_set_vexpand(sw, TRUE); gtk_grid_attach(GTK_GRID(g), sw, 0, 3, 3, 1);
    return panel(g);
}
static void onFilterBtn(GtkWidget* w, gpointer) { setFilter(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "f"))); }
static GtkWidget* tabApps() {
    GtkWidget* v = vbox(); GtkWidget* tb = hbox();
    gSearch = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(gSearch), "search name / package"); gtk_entry_set_width_chars(GTK_ENTRY(gSearch), 26);
    g_signal_connect(gSearch, "changed", G_CALLBACK(onSearch), nullptr); pack(tb, gSearch);
    const char* fn[] = {"ALL", "SAFE", "RISKY"};
    for (int i = 0; i < 3; i++) { GtkWidget* b = btn(fn[i], nullptr, i == 2 ? "danger" : nullptr, false); g_object_set_data(G_OBJECT(b), "f", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(onFilterBtn), nullptr); gFilterBtns.push_back(b); pack(tb, b); }
    addClass(gFilterBtns[0], "on");
    GtkWidget* sp = gtk_label_new(""); gtk_box_pack_start(GTK_BOX(tb), sp, TRUE, TRUE, 0);
    pack(tb, btn("LOAD APPS", G_CALLBACK(onLoad))); pack(tb, btn("DELETE TICKED", G_CALLBACK(onDelete), "danger")); pack(tb, btn("CLEAR", G_CALLBACK(onClear), "dim", false));
    pack(v, tb);
    gAppStore = gtk_list_store_new(C_N, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    gAppView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(gAppStore)); gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(gAppView), TRUE);
    GtkCellRenderer* tr = gtk_cell_renderer_toggle_new(); g_signal_connect(tr, "toggled", G_CALLBACK(onToggle), nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(gAppView), gtk_tree_view_column_new_with_attributes("", tr, "active", C_CHK, nullptr));
    addTextCol(gAppView, "APP", C_NAME, true); addTextCol(gAppView, "PACKAGE", C_PKG, false); addTextCol(gAppView, "SIZE", C_SIZE, false);
    addTextCol(gAppView, "CLASS", C_CLASS, false, true); addTextCol(gAppView, "NEEDS/BY", C_DEPS, false);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(gAppView)), "changed", G_CALLBACK(onAppSel), nullptr);
    GtkWidget* sw = scrolled(gAppView); gtk_widget_set_vexpand(sw, TRUE); pack(v, sw, true);
    gAppStatus = gtk_label_new(""); gtk_widget_set_halign(gAppStatus, GTK_ALIGN_START); pack(v, gAppStatus);
    gDetail = textView(); pack(v, scrolled(gDetail, 120));
    return panel(v);
}
static GtkWidget* tabAdd() {
    GtkWidget* v = vbox(); GtkWidget* tb = hbox();
    pack(tb, btn("ADD APK FILES", G_CALLBACK(onAddFiles), "amber", false)); pack(tb, btn("REMOVE SELECTED", G_CALLBACK(onAddRemove), "dim", false)); pack(tb, btn("CLEAR", G_CALLBACK(onAddClear), "dim", false));
    gPriv = gtk_check_button_new_with_label("PRIV-APP (privileged, writes permission whitelist)"); pack(tb, gPriv);
    GtkWidget* sp = gtk_label_new(""); gtk_box_pack_start(GTK_BOX(tb), sp, TRUE, TRUE, 0); pack(tb, btn("INSTALL INTO IMAGE", G_CALLBACK(onInstall), "danger"));
    pack(v, tb);
    gAddStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING); gAddView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(gAddStore));
    addTextCol(gAddView, "APK FILE", 0, true); addTextCol(gAddView, "SIZE", 1, false);
    GtkWidget* sw = scrolled(gAddView); gtk_widget_set_vexpand(sw, TRUE); pack(v, sw, true);
    GtkWidget* hint = gtk_label_new("tip: drag .apk files from Windows Explorer onto this window, or use ADD APK FILES (Windows drives are under /mnt/c, /mnt/d ...)"); addClass(hint, "dimlbl"); gtk_widget_set_halign(hint, GTK_ALIGN_START); pack(v, hint);
    return panel(v);
}
static GtkWidget* tabProp() {
    GtkWidget* g = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(g), 10); gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gKey = gtk_entry_new(); gVal = gtk_entry_new(); gtk_widget_set_hexpand(gKey, TRUE); gtk_widget_set_hexpand(gVal, TRUE);
    gtk_grid_attach(GTK_GRID(g), lbl("PROPERTY"), 0, 0, 1, 1); gtk_grid_attach(GTK_GRID(g), gKey, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), lbl("VALUE"), 0, 1, 1, 1); gtk_grid_attach(GTK_GRID(g), gVal, 1, 1, 1, 1);
    GtkWidget* hb = hbox(); pack(hb, btn("SET PROPERTY", G_CALLBACK(onSetProp), "danger")); pack(hb, btn("BUMP FINGERPRINT (-muxN)", G_CALLBACK(onBump), "amber")); gtk_grid_attach(GTK_GRID(g), hb, 1, 2, 1, 1);
    GtkWidget* hq = hbox(); for (const char* k : {"ro.product.model", "ro.product.name", "ro.product.device", "ro.product.brand"}) pack(hq, btn(k, G_CALLBACK(onQuick), "dim", false));
    gtk_grid_attach(GTK_GRID(g), hq, 1, 3, 1, 1);
    GtkWidget* sp = gtk_label_new(""); gtk_widget_set_vexpand(sp, TRUE); gtk_grid_attach(GTK_GRID(g), sp, 0, 4, 2, 1);
    return panel(g);
}
static GtkWidget* tabRepack() {
    GtkWidget* v = vbox(); GtkWidget* tb = hbox();
    pack(tb, btn("REPACK system_custom.img", G_CALLBACK(onRepack), "danger")); pack(tb, btn("EXPORT TO FOLDER...", G_CALLBACK(onExport), "amber")); pack(v, tb);
    GtkWidget* g = textView();
    setText(g, "FLASH (fastboot, bootloader unlocked)\n\n1) verification off, pick one:\n   fastboot --disable-verity --disable-verification flash vbmeta google-vbmeta.img\n"
               "   fastboot --disable-verity --disable-verification flash vbmeta stock-vbmeta.img   (plan B)\n\n2) system:\n   fastboot flash system system_custom.img\n\n"
               "3) first boot: factory reset (or bump the fingerprint on the BUILD.PROP tab) so the new system apps get default permissions.\n\n"
               "Keep the stock system.img / vbmeta.img / boot.img safe: Lenovo Rescue can restore them.\n\n"
               "EXPORT copies system_custom.img and system_custom.raw.img to a folder, for example /mnt/d/flash on your Windows drive.");
    GtkWidget* sw = scrolled(g); gtk_widget_set_vexpand(sw, TRUE); pack(v, sw, true); return panel(v);
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    GtkCssProvider* p = gtk_css_provider_new(); gtk_css_provider_load_from_data(p, CSS, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gWin = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(gWin), "MUX // SYSTEM KITCHEN"); gtk_window_set_default_size(GTK_WINDOW(gWin), 1120, 820);
    g_signal_connect(gWin, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget* root = vbox(10); gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    GtkWidget* head = vbox(2); GtkWidget* t = gtk_label_new("MUX // SYSTEM KITCHEN"); addClass(t, "title"); gtk_widget_set_halign(t, GTK_ALIGN_START);
    gHeadState = gtk_label_new("BACKEND: not checked"); addClass(gHeadState, "sub"); gtk_widget_set_halign(gHeadState, GTK_ALIGN_START);
    pack(head, t); pack(head, gHeadState); pack(root, panel(head));
    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tabProject(), gtk_label_new("PROJECT")); gtk_notebook_append_page(GTK_NOTEBOOK(nb), tabApps(), gtk_label_new("APPS"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tabAdd(), gtk_label_new("ADD APPS")); gtk_notebook_append_page(GTK_NOTEBOOK(nb), tabProp(), gtk_label_new("BUILD.PROP"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tabRepack(), gtk_label_new("REPACK"));
    gtk_widget_set_vexpand(nb, TRUE); pack(root, nb, true);
    gProg = gtk_progress_bar_new(); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(gProg), TRUE); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(gProg), "IDLE"); pack(root, gProg);
    gTimer = g_timeout_add(120, tick, nullptr);
    gLog = textView(); GtkWidget* lw = scrolled(gLog, 130); pack(root, lw);
    gtk_container_add(GTK_CONTAINER(gWin), root);
    GtkTargetEntry te = {const_cast<char*>("text/uri-list"), 0, 0}; gtk_drag_dest_set(gWin, GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_COPY);
    g_signal_connect(gWin, "drag-data-received", G_CALLBACK(onDrop), nullptr);
    gtk_widget_show_all(gWin);
    appendLog("Backend: " + selfDir() + "/kitchen   Windows drives are under /mnt/c, /mnt/d ...");
    startJob(K_DOCTOR, {"doctor"});
    gtk_main(); return 0;
}