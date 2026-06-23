// macros.cpp - X11 click-macro builder for DWM
//
// Drop numbered crosshair markers at the mouse position (color = mouse button),
// insert keyboard actions, then replay the ordered sequence with xdotool.
// Overlay via osd_cat, keyboard-payload entry via dmenu.
//
// Build: make   (g++ -O2 -Wall -o macros macros.cpp -lX11)
// Deps : osd_cat (xosd), xdotool, dmenu

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------- model ----------------------------------------------------------

struct Action {
    enum Type { LCLICK, RCLICK, MCLICK, KEYCOMBO, TYPE, WAIT } type;
    int x = 0, y = 0;     // global coords for clicks
    std::string payload;   // for KEYCOMBO / TYPE
    pid_t marker = -1;     // osd_cat: crosshair glyph (-1 = none)
    pid_t markerNum = -1;  // osd_cat: order number
};

static std::vector<Action> g_seq;
static Display *g_dpy = nullptr;
static Window g_root = 0;
static int g_delayMs = 150;  // between actions on replay
static int g_repeat = 1;     // 0 = loop until cancel
static int g_primX = 0;      // primary-monitor origin (osd_cat anchors here)
static int g_primY = 0;

// Marker styling. Crosshair = thin glyph centered on the exact click point;
// number drawn slightly lower-right so it doesn't cover the crosshair.
static const char *CH_FONT  = "-*-*-medium-r-*-*-20-*-*-*-*-*-*-*";  // thin crosshair
static const char *NUM_FONT = "-*-*-bold-r-*-*-18-*-*-*-*-*-*-*";    // order number
static const int CH_CX = 15, CH_CY = 10;   // crosshair centering correction
static const int NUM_DX = 8, NUM_DY = 12;  // number offset from the point

// ---------- helpers --------------------------------------------------------

static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

// Run argv (NULL-terminated), wait for it. Returns child exit status or -1.
static int runWait(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

// Get global pointer position.
static bool getPointer(int *x, int *y) {
    Window r, c;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (!XQueryPointer(g_dpy, g_root, &r, &c, &rx, &ry, &wx, &wy, &mask))
        return false;
    *x = rx;
    *y = ry;
    return true;
}

// Spawn one persistent osd_cat showing `text` at primary-relative (indent, offset)
// in `color` with `font`. Returns pid. osd_cat anchors text top-left at the point.
static pid_t spawnOsd(int indentPx, int offsetPx, const char *color,
                      const char *font, const std::string &text) {
    int fds[2];
    if (pipe(fds) != 0) return -1;

    char offset[16], indent[16];
    snprintf(offset, sizeof offset, "%d", offsetPx);
    snprintf(indent, sizeof indent, "%d", indentPx);

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        const char *argv[] = {
            "osd_cat",
            "--pos=top", "--align=left",
            "--offset", offset, "--indent", indent,
            "--color", color,
            "--outline=2", "--outlinecolour=black",
            "--delay=86400",
            "--font", font,
            nullptr};
        execvp(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    // parent: write the line, keep the pipe open so osd_cat stays alive.
    close(fds[0]);
    std::string line = text + "\n";
    if (write(fds[1], line.data(), line.size()) < 0) { /* ignore */ }
    // intentionally leak fds[1]: closing it would end osd_cat's input.
    return pid;
}

static void killMarker(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

static void killAction(Action &a) {
    killMarker(a.marker);
    killMarker(a.markerNum);
    a.marker = a.markerNum = -1;
}

// ---------- marker styling + serialization --------------------------------

static const char *colorFor(Action::Type t) {
    switch (t) {
        case Action::LCLICK: return "green";
        case Action::RCLICK: return "red";
        case Action::MCLICK: return "cyan";
        case Action::WAIT:   return "white";
        default:             return "yellow";  // KEYCOMBO / TYPE
    }
}

static char glyphFor(Action::Type t) {
    if (t == Action::WAIT) return 'W';
    if (t == Action::KEYCOMBO || t == Action::TYPE) return 'K';
    return '+';
}

static const char *typeName(Action::Type t) {
    switch (t) {
        case Action::LCLICK:   return "LCLICK";
        case Action::RCLICK:   return "RCLICK";
        case Action::MCLICK:   return "MCLICK";
        case Action::KEYCOMBO: return "KEYCOMBO";
        case Action::TYPE:     return "TYPE";
        case Action::WAIT:     return "WAIT";
    }
    return "?";
}

static bool parseType(const std::string &s, Action::Type &t) {
    if (s == "LCLICK") t = Action::LCLICK;
    else if (s == "RCLICK") t = Action::RCLICK;
    else if (s == "MCLICK") t = Action::MCLICK;
    else if (s == "KEYCOMBO") t = Action::KEYCOMBO;
    else if (s == "TYPE") t = Action::TYPE;
    else if (s == "WAIT") t = Action::WAIT;
    else return false;
    return true;
}

// Collect currently-mapped override-redirect windows (osd/menus/etc).
static void collectOR(std::set<Window> &out) {
    Window r, p, *kids = nullptr;
    unsigned int nk = 0;
    if (!XQueryTree(g_dpy, g_root, &r, &p, &kids, &nk)) return;
    for (unsigned int i = 0; i < nk; ++i) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(g_dpy, kids[i], &wa) &&
            wa.map_state == IsViewable && wa.override_redirect)
            out.insert(kids[i]);
    }
    if (kids) XFree(kids);
}

// After spawning markers, our osd_cat windows appear as NEW override-redirect
// windows. Give each an empty input region so clicks pass straight through to the
// app below (matters for manual clicks AND for xdotool replay landing on the app).
static void shapeNewWindows(const std::set<Window> &before, int want) {
    std::set<Window> done;
    for (int t = 0; t < 50 && (int)done.size() < want; ++t) {  // up to ~500ms
        Window r, p, *kids = nullptr;
        unsigned int nk = 0;
        if (XQueryTree(g_dpy, g_root, &r, &p, &kids, &nk)) {
            for (unsigned int i = 0; i < nk; ++i) {
                if (before.count(kids[i]) || done.count(kids[i])) continue;
                XWindowAttributes wa;
                if (!XGetWindowAttributes(g_dpy, kids[i], &wa)) continue;
                if (wa.map_state != IsViewable || !wa.override_redirect) continue;
                XShapeCombineRectangles(g_dpy, kids[i], ShapeInput, 0, 0,
                                        nullptr, 0, ShapeSet, Unsorted);
                done.insert(kids[i]);
            }
            if (kids) XFree(kids);
        }
        if ((int)done.size() < want) { XFlush(g_dpy); msleep(10); }
    }
    XFlush(g_dpy);
}

// Spawn overlay for action `a` numbered `n`: a centered crosshair + an offset number.
static void attachMarker(Action &a, int n) {
    const char *col = colorFor(a.type);
    std::set<Window> before;
    collectOR(before);
    a.marker = spawnOsd(a.x - g_primX - CH_CX, a.y - g_primY - CH_CY,
                        col, CH_FONT, std::string(1, glyphFor(a.type)));
    a.markerNum = spawnOsd(a.x - g_primX + NUM_DX, a.y - g_primY + NUM_DY,
                           col, NUM_FONT, std::to_string(n));
    shapeNewWindows(before, 2);  // make both osd windows click-through
}

// ---------- actions --------------------------------------------------------

static void addClick(Action::Type t) {
    int x, y;
    if (!getPointer(&x, &y)) return;
    Action a;
    a.type = t;
    a.x = x;
    a.y = y;
    g_seq.push_back(a);
    int n = (int)g_seq.size();
    attachMarker(g_seq.back(), n);
    printf("[+] #%d %s @ %d,%d\n", n, typeName(t), x, y);
    fflush(stdout);
}

// Read a payload line from dmenu. Returns false if cancelled/empty.
static bool dmenuPrompt(const char *prompt, std::string &out) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        // dmenu reads choices from stdin; give it none so it just takes typed text.
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        const char *argv[] = {"dmenu", "-p", prompt, nullptr};
        execvp(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    close(fds[1]);
    char buf[1024];
    ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    waitpid(pid, nullptr, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return !out.empty();
}

static void addKey() {
    std::string payload;
    if (!dmenuPrompt("key action ('key:' prefix = combo):", payload)) {
        printf("[ ] key action cancelled\n");
        fflush(stdout);
        return;
    }
    Action a;
    int x = 0, y = 0;
    getPointer(&x, &y);
    a.x = x;
    a.y = y;
    if (payload.rfind("key:", 0) == 0) {
        a.type = Action::KEYCOMBO;
        a.payload = payload.substr(4);
    } else {
        a.type = Action::TYPE;
        a.payload = payload;
    }
    g_seq.push_back(a);
    int n = (int)g_seq.size();
    attachMarker(g_seq.back(), n);
    printf("[+] #%d %s: %s\n", n,
           a.type == Action::KEYCOMBO ? "combo" : "type", a.payload.c_str());
    fflush(stdout);
}

static void addWait() {
    std::string s;
    if (!dmenuPrompt("wait (ms):", s)) {
        printf("[ ] wait cancelled\n");
        fflush(stdout);
        return;
    }
    int ms = atoi(s.c_str());
    if (ms <= 0) { printf("[!] bad wait value\n"); fflush(stdout); return; }
    int x = 0, y = 0;
    getPointer(&x, &y);
    Action a;
    a.type = Action::WAIT;
    a.x = x;
    a.y = y;
    a.payload = std::to_string(ms);
    g_seq.push_back(a);
    int n = (int)g_seq.size();
    attachMarker(g_seq.back(), n);
    printf("[+] #%d wait %dms\n", n, ms);
    fflush(stdout);
}

static void undo() {
    if (g_seq.empty()) return;
    killAction(g_seq.back());
    g_seq.pop_back();
    printf("[-] undo, %zu left\n", g_seq.size());
    fflush(stdout);
}

static void clearAll() {
    for (auto &a : g_seq) killAction(a);
    g_seq.clear();
    printf("[*] cleared\n");
    fflush(stdout);
}

// ---------- save / load (.amc) ---------------------------------------------
// Format: one action per line "TYPE x y payload"; payload = rest of line.
// Lines starting with '#' or blank are ignored.

static std::string withExt(std::string p) {
    if (p.size() < 4 || p.substr(p.size() - 4) != ".amc") p += ".amc";
    return p;
}

static void saveFile(const std::string &rawPath) {
    std::string path = withExt(rawPath);
    std::ofstream f(path);
    if (!f) { printf("[!] cannot write %s\n", path.c_str()); fflush(stdout); return; }
    f << "# amc v1\n";
    for (const auto &a : g_seq)
        f << typeName(a.type) << ' ' << a.x << ' ' << a.y << ' ' << a.payload << '\n';
    printf("[*] saved %zu actions to %s\n", g_seq.size(), path.c_str());
    fflush(stdout);
}

// Load a file into g_seq and spawn markers. Returns false on open failure.
static bool loadFile(const std::string &rawPath) {
    std::string path = withExt(rawPath);
    std::ifstream f(path);
    if (!f) { printf("[!] cannot read %s\n", path.c_str()); fflush(stdout); return false; }
    clearAll();
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string tok;
        Action a;
        if (!(is >> tok) || !parseType(tok, a.type)) continue;
        if (!(is >> a.x >> a.y)) continue;
        is >> std::ws;
        std::getline(is, a.payload);  // rest of line (may contain spaces)
        g_seq.push_back(a);
        attachMarker(g_seq.back(), ++n);
    }
    printf("[*] loaded %d actions from %s\n", n, path.c_str());
    fflush(stdout);
    return true;
}

static void promptSave() {
    std::string p;
    if (!dmenuPrompt("save as (.amc):", p)) { printf("[ ] save cancelled\n"); fflush(stdout); return; }
    saveFile(p);
}

static void promptLoad() {
    std::string p;
    if (!dmenuPrompt("load (.amc):", p)) { printf("[ ] load cancelled\n"); fflush(stdout); return; }
    loadFile(p);
}

static void doAction(const Action &a) {
    switch (a.type) {
        case Action::LCLICK:
        case Action::RCLICK:
        case Action::MCLICK: {
            const char *btn = a.type == Action::LCLICK ? "1"
                            : a.type == Action::RCLICK ? "3" : "2";
            char xs[16], ys[16];
            snprintf(xs, sizeof xs, "%d", a.x);
            snprintf(ys, sizeof ys, "%d", a.y);
            const char *argv[] = {"xdotool", "mousemove", xs, ys, "click", btn, nullptr};
            runWait(argv);
            break;
        }
        case Action::KEYCOMBO: {
            const char *argv[] = {"xdotool", "key", a.payload.c_str(), nullptr};
            runWait(argv);
            break;
        }
        case Action::TYPE: {
            const char *argv[] = {"xdotool", "type", "--", a.payload.c_str(), nullptr};
            runWait(argv);
            break;
        }
        case Action::WAIT:
            msleep(atoi(a.payload.c_str()));
            break;
    }
}

// Non-blocking check: did the user press the cancel key (F11)?
static bool cancelPressed() {
    KeyCode cancel = XKeysymToKeycode(g_dpy, XK_F11);
    XEvent ev;
    bool hit = false;
    while (XPending(g_dpy)) {
        XNextEvent(g_dpy, &ev);
        if (ev.type == KeyPress && ev.xkey.keycode == cancel) hit = true;
    }
    return hit;
}

static void replay(bool prompt) {
    if (g_seq.empty()) {
        printf("[!] nothing to replay\n");
        fflush(stdout);
        return;
    }
    // F9 = run once. Shift+F9 = ask count: blank/Esc = -r default; 0 = loop until F11.
    int repeat = 1;
    if (prompt) {
        repeat = g_repeat;
        std::string s;
        if (dmenuPrompt("repeat (blank=default, 0=loop until F11):", s)) {
            repeat = atoi(s.c_str());
            if (repeat < 0) repeat = 1;
        }
    }
    printf("[>] replay %zu actions, delay=%dms, repeat=%s\n",
           g_seq.size(), g_delayMs, repeat == 0 ? "loop" : std::to_string(repeat).c_str());
    fflush(stdout);

    for (int iter = 0; repeat == 0 || iter < repeat; ++iter) {
        for (size_t i = 0; i < g_seq.size(); ++i) {
            doAction(g_seq[i]);
            if (i + 1 < g_seq.size()) msleep(g_delayMs);
        }
        if (repeat == 0) {
            if (cancelPressed()) { printf("[x] cancelled\n"); break; }
            msleep(g_delayMs);
        }
    }
    fflush(stdout);
}

// ---------- setup ----------------------------------------------------------

static void usage(const char *prog) {
    printf(
        "%s - X11 click-macro builder (DWM)\n\n"
        "Usage: %s [-d ms] [-r n] [-h] [file.amc]\n"
        "  file.amc  load this macro file on startup (markers shown immediately)\n"
        "  -d ms     delay between actions on replay (default 150)\n"
        "  -r n      Shift+F9 default repeat; 0 = loop until F11 (default 1)\n"
        "  -h        this help\n\n"
        "Hotkeys (global):\n"
        "  F1  add LEFT-click marker at cursor   (green)\n"
        "  F2  add RIGHT-click marker at cursor  (red)\n"
        "  F3  add MIDDLE-click marker at cursor (cyan)\n"
        "  F4  add keyboard action via dmenu     (yellow)\n"
        "        'key:ctrl+shift+t' = send combo; anything else = type literally\n"
        "  F5  undo last action\n"
        "  F6  add WAIT action via dmenu (ms)    (white)\n"
        "  F7  save sequence to .amc (dmenu filename)\n"
        "  F8  load .amc (dmenu filename)\n"
        "  F9        replay once\n"
        "  Shift+F9  replay N times (dmenu prompts: blank=-r, 0=loop)\n"
        "  F10 clear all\n"
        "  F11 cancel running loop replay\n"
        "  F12 quit\n",
        prog, prog);
}

int main(int argc, char **argv) {
    std::string loadPath;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) g_delayMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) g_repeat = atoi(argv[++i]);
        else if (argv[i][0] != '-') loadPath = argv[i];
        else { usage(argv[0]); return strcmp(argv[i], "-h") ? 1 : 0; }
    }
    if (g_delayMs < 0) g_delayMs = 0;
    if (g_repeat < 0) g_repeat = 0;

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        fprintf(stderr, "cannot open X display\n");
        return 1;
    }
    g_root = DefaultRootWindow(g_dpy);

    // Find primary monitor origin: osd_cat anchors there, so marker indent/offset
    // are relative to it while pointer/xdotool coords are global.
    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(g_dpy, g_root, True, &nmon);
    if (mons) {
        for (int i = 0; i < nmon; ++i) {
            if (mons[i].primary) { g_primX = mons[i].x; g_primY = mons[i].y; }
        }
        if (nmon > 0 && !g_primX && !g_primY) {  // no flagged primary: use first
            bool any = false;
            for (int i = 0; i < nmon; ++i) if (mons[i].primary) any = true;
            if (!any) { g_primX = mons[0].x; g_primY = mons[0].y; }
        }
        XRRFreeMonitors(mons);
    }
    printf("[*] primary origin: %d,%d\n", g_primX, g_primY);

    if (!loadPath.empty()) loadFile(loadPath);  // markers spawn at saved positions

    // grab F1..F12; dispatch in the loop by keysym.
    KeySym grabbed[] = {XK_F1, XK_F2, XK_F3, XK_F4, XK_F5, XK_F6, XK_F7, XK_F8,
                        XK_F9, XK_F10, XK_F11, XK_F12};
    // Grab with common lock-modifier combos so NumLock/CapsLock don't break it.
    unsigned int locks[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};
    for (KeySym sym : grabbed) {
        KeyCode kc = XKeysymToKeycode(g_dpy, sym);
        if (!kc) continue;
        for (unsigned int m : locks)
            XGrabKey(g_dpy, kc, m, g_root, True, GrabModeAsync, GrabModeAsync);
    }
    // Also grab Shift+F9 (replay with count prompt).
    if (KeyCode f9 = XKeysymToKeycode(g_dpy, XK_F9))
        for (unsigned int m : locks)
            XGrabKey(g_dpy, f9, ShiftMask | m, g_root, True, GrabModeAsync, GrabModeAsync);
    XSelectInput(g_dpy, g_root, KeyPressMask);

    usage(argv[0]);
    printf("\n[*] running. delay=%dms repeat=%s\n",
           g_delayMs, g_repeat == 0 ? "loop" : std::to_string(g_repeat).c_str());
    fflush(stdout);

    bool run = true;
    while (run) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        if (ev.type != KeyPress) continue;
        KeySym sym = XLookupKeysym(&ev.xkey, 0);
        switch (sym) {
            case XK_F1:  addClick(Action::LCLICK); break;
            case XK_F2:  addClick(Action::RCLICK); break;
            case XK_F3:  addClick(Action::MCLICK); break;
            case XK_F4:  addKey(); break;
            case XK_F5:  undo(); break;
            case XK_F6:  addWait(); break;
            case XK_F7:  promptSave(); break;
            case XK_F8:  promptLoad(); break;
            case XK_F9:  replay(ev.xkey.state & ShiftMask); break;
            case XK_F10: clearAll(); break;
            case XK_F11: break;  // only meaningful during a loop replay
            case XK_F12: run = false; break;
            default: break;
        }
    }

    clearAll();
    XCloseDisplay(g_dpy);
    printf("[*] bye\n");
    return 0;
}
