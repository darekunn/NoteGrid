// NoteGrid - a warm, minimal grid notes app for Nintendo Switch homebrew.
// SDL2 + SDL2_ttf + libnx. The same source also builds on PC for quick UI tests.
//
// Controls (NX-Shell style):
//   D-Pad / Left stick  move      A open / select     B back / cancel
//   X options           Y new note (bookmark in note view)
//   -  sort             L / R  filter tabs            ZL / ZR  grid columns
//   +  quit             Touch: tap, drag to scroll, long-press for options

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {

// ------------------------------------------------------------------ constants
constexpr int W = 1280, H = 720;
constexpr int MARGIN = 56, GAP = 22, PAD = 26;
constexpr int GRID_TOP = 214;       // content y where the first card row starts
constexpr int BAR_Y = 26, BAR_D = 68;

const SDL_Color C_BG     = {14, 12, 10, 255};
const SDL_Color C_CARD   = {29, 26, 23, 255};
const SDL_Color C_CARD_HI= {42, 38, 33, 255};
const SDL_Color C_PANEL  = {25, 22, 19, 255};
const SDL_Color C_BTN    = {28, 25, 22, 255};
const SDL_Color C_EDGE   = {50, 46, 40, 255};
const SDL_Color C_CHIP   = {47, 43, 37, 255};
const SDL_Color C_TXT    = {241, 235, 225, 255};
const SDL_Color C_MUTED  = {158, 149, 136, 255};
const SDL_Color C_DIM    = {126, 118, 106, 255};
const SDL_Color C_ACC    = {235, 180, 0, 255};

enum FontId { F_TITLE, F_BODY, F_CHIP, F_BIG, F_HINT, F_SEG, F_VTITLE, F_VBODY, F_MODAL, F_BADGE, F_COUNT };
struct FontSpec { const char* file; int size; };
const FontSpec kFonts[F_COUNT] = {
    {"Poppins-Regular.ttf", 29},  // F_TITLE
    {"Poppins-Light.ttf", 22},    // F_BODY
    {"Poppins-Regular.ttf", 17},  // F_CHIP
    {"Poppins-Light.ttf", 56},    // F_BIG
    {"Poppins-Regular.ttf", 19},  // F_HINT
    {"Poppins-Regular.ttf", 25},  // F_SEG
    {"Poppins-Regular.ttf", 42},  // F_VTITLE
    {"Poppins-Light.ttf", 27},    // F_VBODY
    {"Poppins-Regular.ttf", 25},  // F_MODAL
    {"Poppins-Regular.ttf", 16},  // F_BADGE
};

#ifdef __SWITCH__
const char* kAssetDir = "romfs:/";
#else
const char* kAssetDir = "romfs/";
#endif

// ------------------------------------------------------------------ globals
SDL_Renderer* gR = nullptr;
TTF_Font* gFont[F_COUNT] = {};
bool gRunning = true;

// ------------------------------------------------------------------ utf8
Uint32 decodeUtf8(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    Uint32 cp; int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
    else { cp = 0xFFFD; n = 1; }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += n;
    return cp;
}
std::vector<std::string> splitChars(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t st = i;
        decodeUtf8(s, i);
        out.push_back(s.substr(st, i - st));
    }
    return out;
}
int utf8Count(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

// ------------------------------------------------------------------ AA shape masks
struct Mask {
    int w, h;
    std::vector<float> a;
    Mask(int w_, int h_) : w(w_), h(h_), a((size_t)w_ * h_, 0.f) {}
};
inline float cov(float d) { return std::min(1.f, std::max(0.f, 0.5f - d)); }

void mRoundRect(Mask& m, float x, float y, float w, float h, float r, float stroke = 0) {
    r = std::min(r, std::min(w, h) * 0.5f);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
    int x0 = std::max(0, (int)floorf(x) - 1), x1 = std::min(m.w, (int)ceilf(x + w) + 1);
    int y0 = std::max(0, (int)floorf(y) - 1), y1 = std::min(m.h, (int)ceilf(y + h) + 1);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float qx = fabsf(px + 0.5f - cx) - hx, qy = fabsf(py + 0.5f - cy) - hy;
            float d = hypotf(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
            float c = stroke > 0 ? cov(fabsf(d + stroke * 0.5f) - stroke * 0.5f) : cov(d);
            float& a = m.a[(size_t)py * m.w + px];
            a = std::max(a, c);
        }
}
void mCapsule(Mask& m, float x1, float y1, float x2, float y2, float r) {
    float vx = x2 - x1, vy = y2 - y1, l2 = vx * vx + vy * vy;
    int bx0 = std::max(0, (int)floorf(std::min(x1, x2) - r) - 1), bx1 = std::min(m.w, (int)ceilf(std::max(x1, x2) + r) + 1);
    int by0 = std::max(0, (int)floorf(std::min(y1, y2) - r) - 1), by1 = std::min(m.h, (int)ceilf(std::max(y1, y2) + r) + 1);
    for (int py = by0; py < by1; py++)
        for (int px = bx0; px < bx1; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float t = l2 > 0 ? ((fx - x1) * vx + (fy - y1) * vy) / l2 : 0;
            t = std::min(1.f, std::max(0.f, t));
            float d = hypotf(fx - (x1 + t * vx), fy - (y1 + t * vy)) - r;
            float& a = m.a[(size_t)py * m.w + px];
            a = std::max(a, cov(d));
        }
}
SDL_Texture* maskToTex(const Mask& m) {
    SDL_Texture* t = SDL_CreateTexture(gR, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, m.w, m.h);
    std::vector<Uint32> px((size_t)m.w * m.h);
    for (size_t i = 0; i < px.size(); i++) px[i] = ((Uint32)(m.a[i] * 255.f + 0.5f) << 24) | 0x00FFFFFFu;
    SDL_UpdateTexture(t, nullptr, px.data(), m.w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

std::unordered_map<std::string, SDL_Texture*> gMasks;

SDL_Texture* maskTex(const std::string& key, int w, int h, void (*build)(Mask&, const int*), const int* args) {
    auto it = gMasks.find(key);
    if (it != gMasks.end()) return it->second;
    if (gMasks.size() > 400) {
        for (auto& kv : gMasks) SDL_DestroyTexture(kv.second);
        gMasks.clear();
    }
    Mask m(w, h);
    build(m, args);
    SDL_Texture* t = maskToTex(m);
    gMasks[key] = t;
    return t;
}

void drawTex(SDL_Texture* t, int x, int y, int w, int h, SDL_Color c, int alpha = 255) {
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, (Uint8)std::min(255, alpha * c.a / 255));
    SDL_Rect d = {x, y, w, h};
    SDL_RenderCopy(gR, t, nullptr, &d);
}

// Filled rounded rectangle (r >= min(w,h)/2 gives a pill / circle).
void fillRR(int x, int y, int w, int h, int r, SDL_Color c, int alpha = 255) {
    int a[3] = {w, h, r};
    char key[48];
    snprintf(key, sizeof key, "f%d,%d,%d", w, h, r);
    SDL_Texture* t = maskTex(key, w, h, [](Mask& m, const int* p) { mRoundRect(m, 0, 0, (float)p[0], (float)p[1], (float)p[2]); }, a);
    drawTex(t, x, y, w, h, c, alpha);
}
void ringRR(int x, int y, int w, int h, int r, float stroke, SDL_Color c, int alpha = 255) {
    int a[4] = {w, h, r, (int)(stroke * 10)};
    char key[64];
    snprintf(key, sizeof key, "r%d,%d,%d,%d", w, h, r, a[3]);
    SDL_Texture* t = maskTex(key, w, h, [](Mask& m, const int* p) { mRoundRect(m, 0, 0, (float)p[0], (float)p[1], (float)p[2], p[3] / 10.f); }, a);
    drawTex(t, x, y, w, h, c, alpha);
}

// ------------------------------------------------------------------ icons (drawn on a 40-unit grid)
enum Icon { I_SORT, I_GRID, I_BACK, I_PENCIL, I_BOOKMARK, I_CHECK };
void buildIcon(Mask& m, const int* p) {
    float s = m.w / 40.f;
    auto cap = [&](float x1, float y1, float x2, float y2, float r) { mCapsule(m, x1 * s, y1 * s, x2 * s, y2 * s, r * s); };
    switch (p[0]) {
    case I_SORT:
        cap(9, 13, 31, 13, 1.7f); cap(9, 20, 25, 20, 1.7f); cap(9, 27, 19, 27, 1.7f); break;
    case I_GRID:
        for (int gy = 0; gy < 2; gy++)
            for (int gx = 0; gx < 2; gx++)
                mRoundRect(m, (8 + gx * 14) * s, (8 + gy * 14) * s, 10 * s, 10 * s, 3.2f * s, 2.4f * s);
        break;
    case I_BACK:
        cap(10, 20, 30, 20, 1.7f); cap(19, 11, 10, 20, 1.7f); cap(19, 29, 10, 20, 1.7f); break;
    case I_PENCIL:
        cap(27, 9, 14, 22, 2.6f); cap(14, 22, 11, 29, 1.2f); cap(18, 30.5f, 31, 30.5f, 1.5f); break;
    case I_BOOKMARK:
        cap(12, 8, 28, 8, 1.5f); cap(28, 8, 28, 32, 1.5f); cap(28, 32, 20, 26, 1.5f);
        cap(20, 26, 12, 32, 1.5f); cap(12, 32, 12, 8, 1.5f); break;
    case I_CHECK:
        cap(12, 21, 18, 27, 2.3f); cap(18, 27, 29, 14, 2.3f); break;
    }
}
void drawIcon(Icon ic, int cx, int cy, int size, SDL_Color c, int alpha = 255) {
    int a[1] = {(int)ic};
    char key[32];
    snprintf(key, sizeof key, "i%d,%d", (int)ic, size);
    SDL_Texture* t = maskTex(key, size, size, buildIcon, a);
    drawTex(t, cx - size / 2, cy - size / 2, size, size, c, alpha);
}

// ------------------------------------------------------------------ plain geometry
void fillPoly(const std::vector<SDL_FPoint>& pts, SDL_Color c) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (auto& p : pts) { SDL_Vertex vx; vx.position = p; vx.color = c; vx.tex_coord = {0, 0}; v.push_back(vx); }
    for (size_t i = 1; i + 1 < pts.size(); i++) { idx.push_back(0); idx.push_back((int)i); idx.push_back((int)i + 1); }
    SDL_RenderGeometry(gR, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}
void gradV(int x, int y, int w, int h, SDL_Color top, SDL_Color bot) {
    SDL_Vertex v[4];
    float xs[4] = {(float)x, (float)(x + w), (float)x, (float)(x + w)};
    float ys[4] = {(float)y, (float)y, (float)(y + h), (float)(y + h)};
    for (int i = 0; i < 4; i++) { v[i].position = {xs[i], ys[i]}; v[i].color = i < 2 ? top : bot; v[i].tex_coord = {0, 0}; }
    int idx[6] = {0, 1, 2, 2, 1, 3};
    SDL_RenderGeometry(gR, nullptr, v, 4, idx, 6);
}
void ribbon(int x, int y, int w, int h, SDL_Color c) {
    fillPoly({{(float)x, (float)y}, {(float)(x + w), (float)y}, {(float)(x + w), (float)(y + h)},
              {x + w * 0.5f, (float)(y + h - 10)}, {(float)x, (float)(y + h)}}, c);
}

// ------------------------------------------------------------------ text
std::unordered_map<unsigned long long, int> gAdv;
int advOf(int f, Uint32 cp) {
    unsigned long long k = ((unsigned long long)f << 32) | cp;
    auto it = gAdv.find(k);
    if (it != gAdv.end()) return it->second;
    int minx, maxx, miny, maxy, a = 0;
    if (TTF_GlyphMetrics32(gFont[f], cp, &minx, &maxx, &miny, &maxy, &a) != 0) a = TTF_FontHeight(gFont[f]) / 2;
    gAdv[k] = a;
    return a;
}
int measure(int f, const std::string& s, int tr) {
    if (s.empty()) return 0;
    if (tr == 0) { int w = 0, h = 0; TTF_SizeUTF8(gFont[f], s.c_str(), &w, &h); return w; }
    int w = 0, n = 0;
    size_t i = 0;
    while (i < s.size()) { w += advOf(f, decodeUtf8(s, i)); n++; }
    return w + tr * (n - 1);
}

struct TexRef { SDL_Texture* t; int w, h; };
std::unordered_map<std::string, TexRef> gText;

TexRef getText(int f, const std::string& s, int tr) {
    std::string key = std::to_string(f) + "|" + std::to_string(tr) + "|" + s;
    auto it = gText.find(key);
    if (it != gText.end()) return it->second;
    if (gText.size() > 1500) {
        for (auto& kv : gText) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
        gText.clear();
    }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* surf = nullptr;
    if (!s.empty()) {
        if (tr == 0) {
            surf = TTF_RenderUTF8_Blended(gFont[f], s.c_str(), white);
        } else {
            int total = measure(f, s, tr) + 6, hh = TTF_FontHeight(gFont[f]);
            surf = SDL_CreateRGBSurfaceWithFormat(0, total, hh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_FillRect(surf, nullptr, 0);
                int x = 0;
                for (auto& ch : splitChars(s)) {
                    size_t i = 0;
                    Uint32 cp = decodeUtf8(ch, i);
                    if (cp != ' ') {
                        SDL_Surface* g = TTF_RenderUTF8_Blended(gFont[f], ch.c_str(), white);
                        if (g) { SDL_Rect d = {x, 0, 0, 0}; SDL_BlitSurface(g, nullptr, surf, &d); SDL_FreeSurface(g); }
                    }
                    x += advOf(f, cp) + tr;
                }
            }
        }
    }
    TexRef ref = {nullptr, 0, 0};
    if (surf) {
        ref.t = SDL_CreateTextureFromSurface(gR, surf);
        ref.w = surf->w; ref.h = surf->h;
        if (ref.t) SDL_SetTextureBlendMode(ref.t, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(surf);
    }
    gText[key] = ref;
    return ref;
}
int drawText(int f, const std::string& s, int x, int y, SDL_Color c, int tr = 0, int alpha = 255) {
    TexRef r = getText(f, s, tr);
    if (!r.t) return 0;
    SDL_SetTextureColorMod(r.t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(r.t, (Uint8)std::min(255, alpha * c.a / 255));
    SDL_Rect d = {x, y, r.w, r.h};
    SDL_RenderCopy(gR, r.t, nullptr, &d);
    return r.w;
}
int drawTextC(int f, const std::string& s, int cx, int cy, SDL_Color c, int tr = 0, int alpha = 255) {
    int w = measure(f, s, tr), h = TTF_FontHeight(gFont[f]);
    drawText(f, s, cx - w / 2, cy - h / 2, c, tr, alpha);
    return w;
}

std::vector<std::string> wrapText(int f, const std::string& text, int maxW, int tr) {
    std::vector<std::string> out, words;
    std::string line, w;
    for (char ch : text) {
        if (ch == ' ') { if (!w.empty()) { words.push_back(w); w.clear(); } }
        else w += ch;
    }
    if (!w.empty()) words.push_back(w);
    for (auto& wd : words) {
        std::string cand = line.empty() ? wd : line + " " + wd;
        if (measure(f, cand, tr) <= maxW) { line = cand; continue; }
        if (!line.empty()) { out.push_back(line); line.clear(); }
        if (measure(f, wd, tr) <= maxW) { line = wd; continue; }
        std::string part;
        for (auto& ch : splitChars(wd)) {
            if (!part.empty() && measure(f, part + ch, tr) > maxW) { out.push_back(part); part.clear(); }
            part += ch;
        }
        line = part;
    }
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}
std::string ellipsize(int f, const std::string& s, int maxW, int tr) {
    if (measure(f, s, tr) <= maxW) return s;
    auto ch = splitChars(s);
    while (!ch.empty()) {
        ch.pop_back();
        std::string t;
        for (auto& c : ch) t += c;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        t += "\xE2\x80\xA6";
        if (measure(f, t, tr) <= maxW) return t;
    }
    return "\xE2\x80\xA6";
}

// ------------------------------------------------------------------ notes
struct Note {
    std::string id, title, body;
    int y = 0, m = 0, d = 0;
    bool pin = false;
};
std::vector<Note> gNotes;
std::string gBase, gNotesDir;
int gTab = 0;       // 0 = all, 1 = bookmarks
int gSort = 0;      // 0 newest, 1 oldest, 2 title
int gCols = 3;

void nowLocal(struct tm& out) {
    time_t t = time(nullptr);
#ifdef __SWITCH__
    u64 ts = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts))) { t = (time_t)ts; gmtime_r(&t, &out); return; }
#endif
    localtime_r(&t, &out);
}
std::string chipDate(const Note& n) {
    char b[16];
    snprintf(b, sizeof b, "%02d/%02d", n.m, n.d);
    return b;
}
std::string longDate(const Note& n) {
    static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm t = {};
    t.tm_year = n.y - 1900; t.tm_mon = std::max(0, n.m - 1); t.tm_mday = std::max(1, n.d); t.tm_hour = 12;
    mktime(&t);
    char b[48];
    snprintf(b, sizeof b, "%s, %d %s %d", wd[t.tm_wday % 7], n.d, mo[std::max(0, n.m - 1) % 12], n.y);
    return b;
}

bool dirExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
void ensureDir(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            if (!sub.empty() && sub.back() != ':' && sub.back() != '/') mkdir(sub.c_str(), 0777);
        }
}
bool readFile(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
bool writeFile(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return n == data.size();
}
std::string notePath(const std::string& id) { return gNotesDir + "/" + id + ".txt"; }

bool saveNote(const Note& n) {
    char hdr[160];
    std::string title = n.title;
    for (auto& c : title) if (c == '\n' || c == '\r') c = ' ';
    snprintf(hdr, sizeof hdr, "date: %04d-%02d-%02d\npin: %d\n---\n", n.y, n.m, n.d, n.pin ? 1 : 0);
    return writeFile(notePath(n.id), "title: " + title + "\n" + hdr + n.body);
}
void parseNote(const std::string& id, const std::string& raw, Note& n) {
    n.id = id;
    std::string content;
    for (char c : raw) if (c != '\r') content += c;
    if (content.compare(0, 7, "title: ") != 0) { n.title = id; n.body = content; return; }
    size_t pos = 0;
    while (pos < content.size()) {
        size_t e = content.find('\n', pos);
        if (e == std::string::npos) e = content.size();
        std::string line = content.substr(pos, e - pos);
        pos = e + 1;
        if (line == "---") break;
        if (line.compare(0, 7, "title: ") == 0) n.title = line.substr(7);
        else if (line.compare(0, 6, "date: ") == 0) sscanf(line.c_str() + 6, "%d-%d-%d", &n.y, &n.m, &n.d);
        else if (line.compare(0, 5, "pin: ") == 0) n.pin = atoi(line.c_str() + 5) != 0;
    }
    if (pos < content.size()) n.body = content.substr(pos);
    while (!n.body.empty() && n.body.back() == '\n') n.body.pop_back();
}
void loadNotes() {
    gNotes.clear();
    DIR* d = opendir(gNotesDir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".txt") continue;
        std::string raw;
        if (!readFile(gNotesDir + "/" + name, raw)) continue;
        Note n;
        parseNote(name.substr(0, name.size() - 4), raw, n);
        if (n.y == 0) { struct tm t; nowLocal(t); n.y = t.tm_year + 1900; n.m = t.tm_mon + 1; n.d = t.tm_mday; }
        gNotes.push_back(n);
    }
    closedir(d);
}
void loadSettings() {
    std::string s;
    if (!readFile(gBase + "/settings.txt", s)) return;
    int c = gCols, so = gSort;
    sscanf(s.c_str(), "cols=%d\nsort=%d", &c, &so);
    if (c >= 2 && c <= 4) gCols = c;
    if (so >= 0 && so <= 2) gSort = so;
}
void saveSettings() {
    char b[64];
    snprintf(b, sizeof b, "cols=%d\nsort=%d\n", gCols, gSort);
    writeFile(gBase + "/settings.txt", b);
}
std::string newId() {
    struct tm t; nowLocal(t);
    char b[48];
    snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    std::string id = b;
    for (int k = 2; access(notePath(id).c_str(), F_OK) == 0; k++) id = std::string(b) + "-" + std::to_string(k);
    return id;
}
void addNote(const std::string& title, const std::string& body, bool pin, int y, int m, int d, const char* id = nullptr) {
    Note n;
    n.id = id ? id : newId();
    n.title = title; n.body = body; n.pin = pin; n.y = y; n.m = m; n.d = d;
    saveNote(n);
}
void seedWelcome() {
    struct tm t; nowLocal(t);
    addNote("Welcome",
            "Notes are plain .txt files on your SD card.\n"
            "[x] A opens a note, B goes back\n"
            "[x] Y writes a new note\n"
            "[ ] X opens options: rename, bookmark, delete\n"
            "[ ] - changes sorting, L / R switch tabs",
            true, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}
#ifndef __SWITCH__
void seedDemo() {
    addNote("To do", "[x] Olahraga lima menit\n[ ] Sikat gigi\n[ ] Makan sedikit dan kenyang tepat waktu", true, 2026, 9, 30, "d01");
    addNote("Belanja", "[ ] Sabun cuci\n[ ] Ujung keran di toko bangunan", true, 2026, 10, 9, "d02");
    addNote("Goals", "", true, 2026, 9, 24, "d03");
    addNote("Tugas", "", true, 2026, 9, 24, "d04");
    addNote("Diary", "Hari ini hujan sepanjang sore, jadi aku cuma duduk di dekat jendela sambil menyusun ide untuk homebrew catatan di Switch. Rasanya tenang sekali.", false, 2026, 10, 20, "d05");
    addNote("Money", "Pegangan -54k lalu dapat transferan kecil dari hasil jual barang bekas minggu lalu", true, 2026, 1, 12, "d06");
    addNote("List to do for my switch", "Music:\n[x] Playlist satu\n[x] Playlist dua\n[ ] Playlist tiga", false, 2026, 9, 28, "d07");
    addNote("Check List", "[x] Satu hal kecil yang sudah selesai\n[x] Akhir bulan, cek lagi\n[x] Ternyata bukan mimpi", false, 2026, 10, 20, "d08");
    addNote("Ide aplikasi", "[ ] Tema hangat\n[ ] Grid acak", false, 2026, 9, 24, "d09");
}
#endif

// ------------------------------------------------------------------ view state
std::vector<int> gVis;           // indices into gNotes
std::string gSelId;
int gSel = 0;
bool gDirty = true;

const char* kSortNames[3] = {"Newest first", "Oldest first", "Title A-Z"};

int findNote(const std::string& id) {
    for (size_t i = 0; i < gNotes.size(); i++) if (gNotes[i].id == id) return (int)i;
    return -1;
}
bool noteLess(const Note& a, const Note& b) {
    if (a.pin != b.pin) return a.pin;
    long da = a.y * 10000L + a.m * 100 + a.d, db = b.y * 10000L + b.m * 100 + b.d;
    if (gSort == 2) {
        std::string x = a.title, y = b.title;
        for (auto& c : x) c = (char)tolower((unsigned char)c);
        for (auto& c : y) c = (char)tolower((unsigned char)c);
        if (x != y) return x < y;
        return a.id < b.id;
    }
    if (da != db) return gSort == 0 ? da > db : da < db;
    return gSort == 0 ? a.id > b.id : a.id < b.id;
}
void rebuildVisible() {
    std::vector<int> v;
    for (size_t i = 0; i < gNotes.size(); i++) if (gTab == 0 || gNotes[i].pin) v.push_back((int)i);
    std::sort(v.begin(), v.end(), [](int a, int b) { return noteLess(gNotes[a], gNotes[b]); });
    gVis = v;
    gSel = 0;
    for (size_t i = 0; i < gVis.size(); i++) if (gNotes[gVis[i]].id == gSelId) gSel = (int)i;
    if (!gVis.empty()) gSelId = gNotes[gVis[gSel]].id;
    gDirty = true;
}

// ------------------------------------------------------------------ card layout (masonry)
struct Row { bool check; bool checked; std::string text; };
struct Card {
    int note;
    int x, y, w, h;
    std::vector<std::string> title;
    std::vector<Row> rows;
    bool hasCheck;
    int rowsH;
    std::string chip;
};
std::vector<Card> gCards;
int gContentH = 0;
float gScroll = 0, gScrollT = 0;

constexpr int ROW_TEXT_H = 29, ROW_CHECK_H = 41, PREVIEW_MAX_H = 150;

bool isCheck(const std::string& l, bool& checked, std::string& text) {
    if (l.size() >= 4 && l[0] == '[' && (l[1] == ' ' || l[1] == 'x' || l[1] == 'X') && l[2] == ']' && l[3] == ' ') {
        checked = l[1] != ' ';
        text = l.substr(4);
        return true;
    }
    return false;
}
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}

void layoutGrid() {
    gCards.clear();
    int cw = (W - 2 * MARGIN - GAP * (gCols - 1)) / gCols;
    int inner = cw - 2 * PAD;
    std::vector<int> colY(gCols, GRID_TOP);
    for (int vi = 0; vi < (int)gVis.size(); vi++) {
        const Note& n = gNotes[gVis[vi]];
        Card c;
        c.note = gVis[vi];
        c.w = cw;
        c.chip = chipDate(n);
        c.hasCheck = false;
        int titleW = inner - (n.pin ? 40 : 0);
        c.title = wrapText(F_TITLE, n.title.empty() ? "Untitled" : n.title, titleW, 1);
        if (c.title.size() > 2) {
            c.title.resize(2);
            c.title[1] = ellipsize(F_TITLE, c.title[1] + "\xE2\x80\xA6", titleW, 1);
        }
        // body preview
        std::vector<Row> all;
        for (auto& line : splitLines(n.body)) {
            if (line.empty()) continue;
            bool chk; std::string txt;
            if (isCheck(line, chk, txt)) {
                c.hasCheck = true;
                all.push_back({true, chk, ellipsize(F_BODY, txt, inner - 38, 1)});
            } else {
                for (auto& l : wrapText(F_BODY, line, inner, 1)) all.push_back({false, false, l});
            }
        }
        int used = 0;
        bool cut = false;
        for (auto& r : all) {
            int rh = r.check ? ROW_CHECK_H : ROW_TEXT_H;
            if (used + rh > PREVIEW_MAX_H) { cut = true; break; }
            c.rows.push_back(r);
            used += rh;
        }
        if (cut && !c.rows.empty() && !c.rows.back().check)
            c.rows.back().text = ellipsize(F_BODY, c.rows.back().text + "\xE2\x80\xA6", inner, 1);
        c.rowsH = used;
        c.h = 24 + (int)c.title.size() * 39 + (c.rows.empty() ? 16 : 12 + used) + 16 + 30 + 22;
        int col = (int)(std::min_element(colY.begin(), colY.end()) - colY.begin());
        c.x = MARGIN + col * (cw + GAP);
        c.y = colY[col];
        colY[col] += c.h + GAP;
        gCards.push_back(c);
    }
    gContentH = *std::max_element(colY.begin(), colY.end());
    gDirty = false;
}
float maxScroll() { return std::max(0.f, (float)(gContentH + 140 - H)); }

void ensureVisible() {
    if (gSel < 0 || gSel >= (int)gCards.size()) return;
    const Card& c = gCards[gSel];
    float top = (float)c.y, bot = (float)(c.y + c.h);
    if (c.y <= GRID_TOP) gScrollT = 0;
    else if (top - gScrollT < 128) gScrollT = top - 128;
    if (bot - gScrollT > H - 130) gScrollT = bot - (H - 130);
    gScrollT = std::max(0.f, std::min(gScrollT, maxScroll()));
}
void selectIdx(int i) {
    if (i < 0 || i >= (int)gCards.size()) return;
    gSel = i;
    gSelId = gNotes[gCards[i].note].id;
    ensureVisible();
}
void navigate(int dx, int dy) {
    if (gCards.empty()) return;
    const Card& cur = gCards[gSel];
    int best = -1, bestScore = 1 << 30;
    int ccx = cur.x + cur.w / 2, ccy = cur.y + cur.h / 2;
    for (int i = 0; i < (int)gCards.size(); i++) {
        if (i == gSel) continue;
        const Card& c = gCards[i];
        int cx = c.x + c.w / 2, cy = c.y + c.h / 2;
        int score;
        if (dy != 0) {
            if (abs(cx - ccx) > cur.w / 2) continue;               // same column only
            int d = (cy - ccy) * dy;
            if (d <= 0) continue;
            score = d;
        } else {
            int d = (cx - ccx) * dx;
            if (d <= 0) continue;
            score = d * 4 + abs(cy - ccy);
        }
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) selectIdx(best);
    else if (dy > 0 && gSel + 1 < (int)gCards.size()) selectIdx(gSel + 1);   // wrap to next card when column ends
    else if (dy < 0 && gSel > 0) selectIdx(gSel - 1);
}

// ------------------------------------------------------------------ note viewer layout
struct VRow { int kind; bool checked; std::vector<std::string> lines; int h; };  // kind 0 text, 1 check, 2 gap
std::vector<VRow> gVRows;
std::vector<std::string> gVTitle;
int gVH = 0;
bool gVDirty = true;
float gVScroll = 0, gVScrollT = 0;
constexpr int VW = 880, VLINE = 41;
int vLeft() { return (W - VW) / 2; }

enum Screen { S_GRID, S_VIEW };
Screen gScreen = S_GRID;
std::string gViewId;

void layoutView() {
    gVRows.clear();
    int i = findNote(gViewId);
    if (i < 0) return;
    const Note& n = gNotes[i];
    gVTitle = wrapText(F_VTITLE, n.title.empty() ? "Untitled" : n.title, VW, 1);
    gVH = 128 + (int)gVTitle.size() * 58 + 24;
    for (auto& line : splitLines(n.body)) {
        VRow r;
        bool chk; std::string txt;
        if (line.empty()) { r.kind = 2; r.checked = false; r.h = 16; }
        else if (isCheck(line, chk, txt)) {
            r.kind = 1; r.checked = chk;
            r.lines = wrapText(F_VBODY, txt, VW - 50, 1);
            r.h = (int)r.lines.size() * VLINE + 8;
        } else {
            r.kind = 0; r.checked = false;
            r.lines = wrapText(F_VBODY, line, VW, 1);
            r.h = (int)r.lines.size() * VLINE + 4;
        }
        gVH += r.h;
        gVRows.push_back(r);
    }
    gVH += 40;
    gVDirty = false;
}
float vMaxScroll() { return std::max(0.f, (float)(gVH + 130 - H)); }

// ------------------------------------------------------------------ toast + modal state
std::string gToast;
double gToastT = 0;
void toast(const std::string& s) { gToast = s; gToastT = 1.9; }

enum Modal { M_NONE, M_OPTIONS, M_CONFIRM };
enum OptCode { O_EDIT, O_RENAME, O_PIN, O_DELETE, O_CONFIRM_DELETE, O_CANCEL };
int gModal = M_NONE, gOptSel = 0;
std::vector<std::string> gOptItems;
std::vector<int> gOptCodes;
std::string gOptTitle, gOptNoteId;

// ------------------------------------------------------------------ text input
bool promptText(const char* header, const char* guide, const std::string& initial, int maxChars, bool multiline, std::string& out) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) { toast("Could not open the on-screen keyboard"); return false; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, maxChars);
    swkbdConfigSetReturnButtonFlag(&kbd, multiline ? 1 : 0);
    static char buf[2048];
    buf[0] = 0;
    Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out = buf;
    return true;
#else
    (void)header; (void)guide; (void)initial; (void)maxChars; (void)multiline; (void)out;
    toast("On-screen keyboard is Switch only");
    return false;
#endif
}
std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) i++;
    return s.substr(i);
}

// ------------------------------------------------------------------ actions
void commitChange(const std::string& id) {
    int i = findNote(id);
    if (i >= 0 && !saveNote(gNotes[i])) toast("Could not save to the SD card");
    gSelId = id;
    rebuildVisible();
    gVDirty = true;
}
void actNew() {
    std::string t, b;
    if (!promptText("New note", "Title", "", 60, false, t)) return;
    t = trimmed(t);
    if (t.empty()) t = "Untitled";
    promptText("Note text", "Write something (optional)", "", 500, true, b);
    struct tm now; nowLocal(now);
    Note n;
    n.id = newId(); n.title = t; n.body = trimmed(b);
    n.y = now.tm_year + 1900; n.m = now.tm_mon + 1; n.d = now.tm_mday;
    gNotes.push_back(n);
    commitChange(n.id);
    toast("Saved");
}
void actEditBody(const std::string& id) {
    int i = findNote(id);
    if (i < 0) return;
    std::string out;
    if (utf8Count(gNotes[i].body) <= 500) {
        if (!promptText("Edit note", "Note text", gNotes[i].body, 500, true, out)) return;
        gNotes[i].body = trimmed(out);
    } else {
        // Switch keyboard is capped at 500 characters; append instead of risking truncation.
        if (!promptText("Add to note", "Text is added at the end", "", 500, true, out)) return;
        out = trimmed(out);
        if (out.empty()) return;
        gNotes[i].body += "\n" + out;
    }
    commitChange(id);
    toast("Saved");
}
void actRename(const std::string& id) {
    int i = findNote(id);
    if (i < 0) return;
    std::string out;
    if (!promptText("Rename note", "Title", gNotes[i].title, 60, false, out)) return;
    out = trimmed(out);
    gNotes[i].title = out.empty() ? "Untitled" : out;
    commitChange(id);
    toast("Renamed");
}
void actPin(const std::string& id) {
    int i = findNote(id);
    if (i < 0) return;
    gNotes[i].pin = !gNotes[i].pin;
    bool pinned = gNotes[i].pin;
    commitChange(id);
    toast(pinned ? "Bookmarked" : "Bookmark removed");
}
void actDelete(const std::string& id) {
    int i = findNote(id);
    if (i < 0) return;
    remove(notePath(id).c_str());
    gNotes.erase(gNotes.begin() + i);
    int keep = std::min(gSel, std::max(0, (int)gVis.size() - 2));
    rebuildVisible();
    if (!gVis.empty()) { gSel = std::min(keep, (int)gVis.size() - 1); gSelId = gNotes[gVis[gSel]].id; }
    gScreen = S_GRID;
    gDirty = true;
    toast("Note deleted");
}
void openNote(const std::string& id) {
    gViewId = id;
    gScreen = S_VIEW;
    gVDirty = true;
    gVScroll = gVScrollT = 0;
}
void openOptions(const std::string& id) {
    int i = findNote(id);
    if (i < 0) return;
    gOptNoteId = id;
    gOptTitle = gNotes[i].title;
    gOptItems = {"Edit text", "Rename", gNotes[i].pin ? "Remove bookmark" : "Bookmark", "Delete"};
    gOptCodes = {O_EDIT, O_RENAME, O_PIN, O_DELETE};
    gOptSel = 0;
    gModal = M_OPTIONS;
}
void execOption(int code) {
    std::string id = gOptNoteId;
    gModal = M_NONE;
    switch (code) {
    case O_EDIT: actEditBody(id); break;
    case O_RENAME: actRename(id); break;
    case O_PIN: actPin(id); break;
    case O_DELETE:
        gOptItems = {"Delete note", "Cancel"};
        gOptCodes = {O_CONFIRM_DELETE, O_CANCEL};
        gOptSel = 1;
        gModal = M_CONFIRM;
        break;
    case O_CONFIRM_DELETE: actDelete(id); break;
    default: break;
    }
}
void cycleSort() {
    gSort = (gSort + 1) % 3;
    saveSettings();
    rebuildVisible();
    toast(kSortNames[gSort]);
}
void changeCols(int delta) {
    int c = gCols + delta;
    if (c < 2) c = 4;
    if (c > 4) c = 2;
    gCols = c;
    saveSettings();
    gDirty = true;
}
void setTab(int t) {
    if (gTab == t) return;
    gTab = t;
    gScroll = gScrollT = 0;
    rebuildVisible();
}

// ------------------------------------------------------------------ drawing: chrome
void circleBtn(int x, int y, Icon ic, bool accent = false) {
    fillRR(x, y, BAR_D, BAR_D, BAR_D / 2, C_BTN);
    ringRR(x, y, BAR_D, BAR_D, BAR_D / 2, 1.6f, C_EDGE);
    drawIcon(ic, x + BAR_D / 2, y + BAR_D / 2, 38, accent ? C_ACC : C_TXT);
}
void fadeEdges() {
    SDL_Color a = C_BG, z = C_BG;
    a.a = 255; z.a = 0;
    gradV(0, 0, W, 96, a, a);
    gradV(0, 96, W, 54, a, z);
    gradV(0, H - 200, W, 110, z, a);
    gradV(0, H - 90, W, 90, a, a);
}
int hintWidth(const std::string& key, const std::string& label) {
    int bw = std::max(30, measure(F_BADGE, key, 0) + 18);
    return bw + 10 + measure(F_HINT, label, 1);
}
void drawHints(const std::vector<std::pair<std::string, std::string>>& hints, int x, int y, int h) {
    int total = 40;
    for (size_t i = 0; i < hints.size(); i++) total += hintWidth(hints[i].first, hints[i].second) + (i + 1 < hints.size() ? 26 : 0);
    fillRR(x, y, total, h, h / 2, C_BTN);
    ringRR(x, y, total, h, h / 2, 1.6f, C_EDGE);
    int cx = x + 20, cy = y + h / 2;
    for (auto& hp : hints) {
        int bw = std::max(30, measure(F_BADGE, hp.first, 0) + 18);
        fillRR(cx, cy - 15, bw, 30, 15, C_CHIP);
        drawTextC(F_BADGE, hp.first, cx + bw / 2, cy, C_TXT);
        cx += bw + 10;
        drawText(F_HINT, hp.second, cx, cy - TTF_FontHeight(gFont[F_HINT]) / 2, C_MUTED, 1);
        cx += measure(F_HINT, hp.second, 1) + 26;
    }
}
constexpr int FAB_D = 72;
int fabX() { return W - MARGIN - FAB_D; }
int fabY() { return H - 48 - FAB_D; }
void drawFab(Icon ic) {
    fillRR(fabX(), fabY(), FAB_D, FAB_D, FAB_D / 2, C_BTN);
    ringRR(fabX(), fabY(), FAB_D, FAB_D, FAB_D / 2, 1.6f, C_EDGE);
    drawIcon(ic, fabX() + FAB_D / 2, fabY() + FAB_D / 2, 40, C_TXT);
}
void drawToast() {
    if (gToastT <= 0) return;
    int a = (int)(255 * std::min(1.0, gToastT / 0.4));
    int w = measure(F_HINT, gToast, 1) + 56, h = 52;
    int x = (W - w) / 2, y = H - 190;
    fillRR(x, y, w, h, h / 2, C_CHIP, a);
    drawTextC(F_HINT, gToast, W / 2, y + h / 2, C_TXT, 1, a);
}

// ------------------------------------------------------------------ drawing: grid
SDL_Color cardTint(const std::string& id) {
    unsigned h = 2166136261u;
    for (char c : id) h = (h ^ (unsigned char)c) * 16777619u;
    static const int dr[5] = {0, 2, -2, 3, -1}, dg[5] = {0, 1, -1, 0, 1}, db[5] = {0, -2, 1, -1, 2};
    int k = (h >> 8) % 5;
    return {(Uint8)(C_CARD.r + dr[k]), (Uint8)(C_CARD.g + dg[k]), (Uint8)(C_CARD.b + db[k]), 255};
}
void drawCheckCircle(int x, int y, int d, bool checked) {
    if (checked) {
        fillRR(x, y, d, d, d / 2, C_ACC);
        drawIcon(I_CHECK, x + d / 2, y + d / 2, d + 6, C_BG);
    } else {
        ringRR(x, y, d, d, d / 2, 2.2f, C_MUTED);
    }
}
void drawCard(const Card& c, int cy, bool sel) {
    const Note& n = gNotes[c.note];
    int x = c.x;
    if (sel) ringRR(x - 5, cy - 5, c.w + 10, c.h + 10, 31, 3.0f, C_ACC);
    fillRR(x, cy, c.w, c.h, 26, sel ? C_CARD_HI : cardTint(n.id));
    int ty = cy + 22;
    for (auto& l : c.title) { drawText(F_TITLE, l, x + PAD, ty, C_TXT, 1); ty += 39; }
    int ry = ty + (c.rows.empty() ? 0 : 6);
    for (auto& r : c.rows) {
        if (r.check) {
            drawCheckCircle(x + PAD, ry + 8, 24, r.checked);
            drawText(F_BODY, r.text, x + PAD + 38, ry + 3, r.checked ? C_DIM : C_TXT, 1);
            ry += ROW_CHECK_H;
        } else {
            drawText(F_BODY, r.text, x + PAD, ry, C_MUTED, 1);
            ry += ROW_TEXT_H;
        }
    }
    int chipY = cy + c.h - 22 - 30;
    int cwid = measure(F_CHIP, c.chip, 1) + 26;
    fillRR(x + PAD, chipY, cwid, 30, 10, C_CHIP);
    drawTextC(F_CHIP, c.chip, x + PAD + cwid / 2, chipY + 15, C_DIM, 1);
    if (c.hasCheck) {
        int ix = x + PAD + cwid + 8;
        fillRR(ix, chipY, 30, 30, 10, C_CHIP);
        drawIcon(I_CHECK, ix + 15, chipY + 15, 30, C_DIM);
    }
    if (n.pin) ribbon(x + c.w - 52, cy, 24, 38, C_ACC);
}
void segRect(int& x, int& y, int& w, int& h) { w = 400; h = BAR_D; x = (W - w) / 2; y = BAR_Y; }

void drawGrid() {
    int sy = (int)lroundf(gScroll);
    drawText(F_BIG, gTab == 0 ? "All notes" : "Bookmarks", MARGIN + 2, 104 - sy, C_TXT, 4);
    for (int i = 0; i < (int)gCards.size(); i++) {
        const Card& c = gCards[i];
        int cy = c.y - sy;
        if (cy > H || cy + c.h < -10) continue;
        drawCard(c, cy, i == gSel);
    }
    if (gCards.empty()) {
        drawTextC(F_VTITLE, gTab == 0 ? "No notes yet" : "No bookmarks yet", W / 2, 330, C_MUTED, 2);
        drawTextC(F_HINT, gTab == 0 ? "Press Y or tap the pencil to write one" : "Press X on a note and choose Bookmark", W / 2, 384, C_DIM, 1);
    }
    fadeEdges();
    circleBtn(MARGIN, BAR_Y, I_SORT);
    circleBtn(W - MARGIN - BAR_D, BAR_Y, I_GRID);
    int sx, sy2, sw, sh;
    segRect(sx, sy2, sw, sh);
    fillRR(sx, sy2, sw, sh, sh / 2, C_BTN);
    ringRR(sx, sy2, sw, sh, sh / 2, 1.6f, C_EDGE);
    fillRR(sx + 6 + (gTab ? sw / 2 - 6 : 0), sy2 + 6, sw / 2, sh - 12, (sh - 12) / 2, C_CHIP);
    drawTextC(F_SEG, "Notes", sx + sw / 4 + 3, sy2 + sh / 2, gTab == 0 ? C_ACC : C_TXT, 1);
    drawTextC(F_SEG, "Bookmarks", sx + 3 * sw / 4, sy2 + sh / 2, gTab == 1 ? C_ACC : C_TXT, 1);
    drawFab(I_PENCIL);
}

// ------------------------------------------------------------------ drawing: viewer
void drawView() {
    int ni = findNote(gViewId);
    if (ni < 0) { gScreen = S_GRID; return; }
    const Note& n = gNotes[ni];
    int sy = (int)lroundf(gVScroll), L = vLeft();
    int y = 118 - sy;
    for (auto& l : gVTitle) { drawText(F_VTITLE, l, L, y, C_TXT, 1); y += 58; }
    y += 24;
    for (auto& r : gVRows) {
        if (r.kind == 1) {
            drawCheckCircle(L, y + 8, 26, r.checked);
            for (size_t i = 0; i < r.lines.size(); i++)
                drawText(F_VBODY, r.lines[i], L + 46, y + 2 + (int)i * VLINE, r.checked ? C_DIM : C_TXT, 1);
        } else if (r.kind == 0) {
            for (size_t i = 0; i < r.lines.size(); i++)
                drawText(F_VBODY, r.lines[i], L, y + (int)i * VLINE, C_MUTED, 1);
        }
        y += r.h;
    }
    if (n.body.empty()) drawText(F_VBODY, "Empty note - press A to write", L, y, C_DIM, 1);
    fadeEdges();
    circleBtn(MARGIN, BAR_Y, I_BACK);
    // date pill
    std::string ds = longDate(n);
    int dw = measure(F_SEG, ds, 1) + 64;
    fillRR((W - dw) / 2, BAR_Y, dw, BAR_D, BAR_D / 2, C_BTN);
    ringRR((W - dw) / 2, BAR_Y, dw, BAR_D, BAR_D / 2, 1.6f, C_EDGE);
    drawTextC(F_SEG, ds, W / 2, BAR_Y + BAR_D / 2, C_MUTED, 1);
    // bookmark button
    int bx = W - MARGIN - BAR_D;
    fillRR(bx, BAR_Y, BAR_D, BAR_D, BAR_D / 2, C_BTN);
    ringRR(bx, BAR_Y, BAR_D, BAR_D, BAR_D / 2, 1.6f, C_EDGE);
    if (n.pin) ribbon(bx + BAR_D / 2 - 11, BAR_Y + BAR_D / 2 - 17, 22, 34, C_ACC);
    else drawIcon(I_BOOKMARK, bx + BAR_D / 2, BAR_Y + BAR_D / 2, 38, C_TXT);
    drawFab(I_PENCIL);
}

// ------------------------------------------------------------------ drawing: modal
constexpr int OPT_W = 480, OPT_ROW = 60;
void modalRect(int& x, int& y, int& w, int& h) {
    w = OPT_W;
    h = 24 + 52 + (int)gOptItems.size() * OPT_ROW + 18;
    x = (W - w) / 2;
    y = (H - h) / 2;
}
void drawModal() {
    if (gModal == M_NONE) return;
    SDL_SetRenderDrawColor(gR, 0, 0, 0, 175);
    SDL_Rect full = {0, 0, W, H};
    SDL_RenderFillRect(gR, &full);
    int x, y, w, h;
    modalRect(x, y, w, h);
    fillRR(x, y, w, h, 30, C_PANEL);
    ringRR(x, y, w, h, 30, 1.6f, C_EDGE);
    std::string head = gModal == M_CONFIRM ? "Delete this note?" : ellipsize(F_MODAL, gOptTitle, w - 64, 1);
    drawTextC(F_MODAL, head, x + w / 2, y + 24 + 24, C_MUTED, 1);
    for (size_t i = 0; i < gOptItems.size(); i++) {
        int ry = y + 24 + 52 + (int)i * OPT_ROW;
        bool sel = (int)i == gOptSel;
        if (sel) fillRR(x + 16, ry, w - 32, OPT_ROW - 8, (OPT_ROW - 8) / 2, C_CHIP);
        bool danger = gOptCodes[i] == O_DELETE || gOptCodes[i] == O_CONFIRM_DELETE;
        SDL_Color col = sel ? C_ACC : (danger ? SDL_Color{225, 120, 100, 255} : C_TXT);
        drawTextC(F_MODAL, gOptItems[i], x + w / 2, ry + (OPT_ROW - 8) / 2, col, 1);
    }
}
// Bottom button hints are drawn last so they stay crisp above the modal dimmer.
void drawBottomHints() {
    std::vector<std::pair<std::string, std::string>> h;
    if (gModal != M_NONE) h = {{"A", "Select"}, {"B", "Cancel"}};
    else if (gScreen == S_GRID) h = {{"A", "Open"}, {"X", "Options"}, {"Y", "New"}, {"-", "Sort"}, {"L R", "Tabs"}, {"+", "Quit"}};
    else h = {{"A", "Edit"}, {"X", "Options"}, {"Y", "Bookmark"}, {"B", "Back"}};
    drawHints(h, MARGIN, fabY() + 4, 64);
}

// ------------------------------------------------------------------ input
struct In {
    bool up = 0, down = 0, left = 0, right = 0, a = 0, b = 0, x = 0, y = 0, l = 0, r = 0, zl = 0, zr = 0, plus = 0, minus = 0;
    float stick = 0;   // vertical analog (-1 up .. +1 down), continuous
};
In gPending;          // set by keyboard events (PC) or scripts

#ifdef __SWITCH__
PadState gPad;
struct Rep { double t = 0; bool held = false; };
bool repeatFire(Rep& r, bool now, double dt) {
    if (!now) { r.held = false; r.t = 0; return false; }
    if (!r.held) { r.held = true; r.t = 0.38; return true; }
    r.t -= dt;
    if (r.t <= 0) { r.t = 0.075; return true; }
    return false;
}
#endif

void pollInput(In& in, double dt) {
    in = gPending;
    gPending = In();
#ifdef __SWITCH__
    static Rep ru, rd, rl, rr;
    padUpdate(&gPad);
    u64 dn = padGetButtonsDown(&gPad), hd = padGetButtons(&gPad);
    HidAnalogStickState ls = padGetStickPos(&gPad, 0);
    bool su = ls.y > 16000, sd = ls.y < -16000, sl = ls.x < -16000, sr = ls.x > 16000;
    in.up |= repeatFire(ru, (hd & HidNpadButton_Up) || su, dt);
    in.down |= repeatFire(rd, (hd & HidNpadButton_Down) || sd, dt);
    in.left |= repeatFire(rl, (hd & HidNpadButton_Left) || sl, dt);
    in.right |= repeatFire(rr, (hd & HidNpadButton_Right) || sr, dt);
    in.a |= (dn & HidNpadButton_A) != 0; in.b |= (dn & HidNpadButton_B) != 0;
    in.x |= (dn & HidNpadButton_X) != 0; in.y |= (dn & HidNpadButton_Y) != 0;
    in.l |= (dn & HidNpadButton_L) != 0; in.r |= (dn & HidNpadButton_R) != 0;
    in.zl |= (dn & HidNpadButton_ZL) != 0; in.zr |= (dn & HidNpadButton_ZR) != 0;
    in.plus |= (dn & HidNpadButton_Plus) != 0; in.minus |= (dn & HidNpadButton_Minus) != 0;
    in.stick = -(float)ls.y / 32767.f;
    if (fabsf(in.stick) < 0.3f) in.stick = 0;
#else
    (void)dt;
#endif
}

// touch
struct Touch {
    bool down = false;
    SDL_FingerID id = 0;
    float sx = 0, sy = 0, lx = 0, ly = 0, vy = 0;
    bool dragged = false, longFired = false;
    Uint32 t0 = 0;
} gTouch;

bool inRect(float px, float py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

int cardAt(float px, float py) {
    int sy = (int)lroundf(gScroll);
    for (int i = 0; i < (int)gCards.size(); i++) {
        const Card& c = gCards[i];
        if (inRect(px, py, c.x, c.y - sy, c.w, c.h)) return i;
    }
    return -1;
}
void tapModal(float px, float py) {
    int x, y, w, h;
    modalRect(x, y, w, h);
    if (!inRect(px, py, x, y, w, h)) { gModal = M_NONE; return; }
    for (size_t i = 0; i < gOptItems.size(); i++) {
        int ry = y + 24 + 52 + (int)i * OPT_ROW;
        if (inRect(px, py, x + 16, ry, w - 32, OPT_ROW - 8)) { gOptSel = (int)i; execOption(gOptCodes[i]); return; }
    }
}
void tapGrid(float px, float py) {
    int sx, sy, sw, sh;
    segRect(sx, sy, sw, sh);
    if (inRect(px, py, MARGIN, BAR_Y, BAR_D, BAR_D)) { cycleSort(); return; }
    if (inRect(px, py, W - MARGIN - BAR_D, BAR_Y, BAR_D, BAR_D)) { changeCols(1); return; }
    if (inRect(px, py, sx, sy, sw, sh)) { setTab(px < sx + sw / 2 ? 0 : 1); return; }
    if (inRect(px, py, fabX(), fabY(), FAB_D, FAB_D)) { actNew(); return; }
    int i = cardAt(py < 150 ? -1 : px, py);
    if (i >= 0 && py < H - 90) { selectIdx(i); openNote(gNotes[gCards[i].note].id); }
}
void tapView(float px, float py) {
    if (inRect(px, py, MARGIN, BAR_Y, BAR_D, BAR_D)) { gScreen = S_GRID; return; }
    if (inRect(px, py, W - MARGIN - BAR_D, BAR_Y, BAR_D, BAR_D)) { actPin(gViewId); return; }
    if (inRect(px, py, fabX(), fabY(), FAB_D, FAB_D)) { actEditBody(gViewId); return; }
}
void touchScroll(float dy) {
    if (gScreen == S_GRID) { gScroll = std::max(0.f, std::min(gScroll - dy, maxScroll())); gScrollT = gScroll; }
    else { gVScroll = std::max(0.f, std::min(gVScroll - dy, vMaxScroll())); gVScrollT = gVScroll; }
}
void handleFinger(const SDL_Event& e) {
    float px = e.tfinger.x * W, py = e.tfinger.y * H;
    if (e.type == SDL_FINGERDOWN && !gTouch.down) {
        gTouch = Touch();
        gTouch.down = true; gTouch.id = e.tfinger.fingerId;
        gTouch.sx = gTouch.lx = px; gTouch.sy = gTouch.ly = py;
        gTouch.t0 = SDL_GetTicks();
    } else if (e.type == SDL_FINGERMOTION && gTouch.down && e.tfinger.fingerId == gTouch.id) {
        if (gModal == M_NONE) {
            if (!gTouch.dragged && fabsf(py - gTouch.sy) > 12) gTouch.dragged = true;
            if (gTouch.dragged) { touchScroll(py - gTouch.ly); gTouch.vy = 0.7f * gTouch.vy + 0.3f * (gTouch.ly - py) * 60.f; }
        }
        gTouch.lx = px; gTouch.ly = py;
    } else if (e.type == SDL_FINGERUP && gTouch.down && e.tfinger.fingerId == gTouch.id) {
        gTouch.down = false;
        if (gModal != M_NONE) { if (!gTouch.dragged) tapModal(px, py); return; }
        if (gTouch.dragged) {
            if (gScreen == S_GRID) gScrollT = std::max(0.f, std::min(gScroll + gTouch.vy * 0.28f, maxScroll()));
            else gVScrollT = std::max(0.f, std::min(gVScroll + gTouch.vy * 0.28f, vMaxScroll()));
        } else if (!gTouch.longFired) {
            if (gScreen == S_GRID) tapGrid(px, py); else tapView(px, py);
        }
    }
}
void updateLongPress() {
    if (!gTouch.down || gTouch.dragged || gTouch.longFired || gModal != M_NONE) return;
    if (SDL_GetTicks() - gTouch.t0 < 550) return;
    if (gScreen == S_GRID) {
        int i = cardAt(gTouch.sx, gTouch.sy);
        if (i >= 0 && gTouch.sy > 150 && gTouch.sy < H - 90) {
            gTouch.longFired = true;
            selectIdx(i);
            openOptions(gNotes[gCards[i].note].id);
        }
    }
}

// ------------------------------------------------------------------ update
void updateGrid(const In& in) {
    if (in.l) setTab(0);
    if (in.r) setTab(1);
    if (in.minus) cycleSort();
    if (in.zl) changeCols(-1);
    if (in.zr) changeCols(1);
    if (in.y) { actNew(); return; }
    if (gCards.empty()) return;
    if (in.up) navigate(0, -1);
    if (in.down) navigate(0, 1);
    if (in.left) navigate(-1, 0);
    if (in.right) navigate(1, 0);
    if (in.a) openNote(gNotes[gCards[gSel].note].id);
    if (in.x) openOptions(gNotes[gCards[gSel].note].id);
}
void updateView(const In& in, double dt) {
    if (in.b) { gScreen = S_GRID; return; }
    if (in.a) { actEditBody(gViewId); return; }
    if (in.x) { openOptions(gViewId); return; }
    if (in.y) { actPin(gViewId); return; }
    if (in.up) gVScrollT -= 120;
    if (in.down) gVScrollT += 120;
    if (in.stick != 0) { gVScroll += in.stick * (float)dt * 900.f; gVScrollT = gVScroll; }
    gVScrollT = std::max(0.f, std::min(gVScrollT, vMaxScroll()));
    gVScroll = std::max(0.f, std::min(gVScroll, vMaxScroll()));
}
void updateModal(const In& in) {
    int n = (int)gOptItems.size();
    if (in.up) gOptSel = (gOptSel + n - 1) % n;
    if (in.down) gOptSel = (gOptSel + 1) % n;
    if (in.b) { gModal = M_NONE; return; }
    if (in.a) execOption(gOptCodes[gOptSel]);
}

#ifndef __SWITCH__
void keyToPending(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: gPending.up = 1; break;
    case SDLK_DOWN: gPending.down = 1; break;
    case SDLK_LEFT: gPending.left = 1; break;
    case SDLK_RIGHT: gPending.right = 1; break;
    case SDLK_RETURN: case SDLK_SPACE: gPending.a = 1; break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: gPending.b = 1; break;
    case SDLK_x: gPending.x = 1; break;
    case SDLK_y: gPending.y = 1; break;
    case SDLK_q: gPending.l = 1; break;
    case SDLK_e: gPending.r = 1; break;
    case SDLK_z: gPending.zl = 1; break;
    case SDLK_c: gPending.zr = 1; break;
    case SDLK_MINUS: gPending.minus = 1; break;
    case SDLK_EQUALS: gPending.plus = 1; break;
    }
}
void tokenToPending(const std::string& t) {
    static const std::pair<const char*, SDL_Keycode> m[] = {
        {"up", SDLK_UP}, {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT}, {"a", SDLK_RETURN}, {"b", SDLK_ESCAPE},
        {"x", SDLK_x}, {"y", SDLK_y}, {"l", SDLK_q}, {"r", SDLK_e}, {"zl", SDLK_z}, {"zr", SDLK_c}, {"minus", SDLK_MINUS}};
    for (auto& kv : m) if (t == kv.first) keyToPending(kv.second);
}
#endif

}  // namespace

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::string shotPath, keyScript;
    bool demo = false;
#ifdef __SWITCH__
    romfsInit();
    timeInitialize();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);
    gBase = "sdmc:/switch/notegrid";
#else
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--demo") demo = true;
        else if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--keys" && i + 1 < argc) keyScript = argv[++i];
    }
    const char* envDir = getenv("NOTEGRID_DIR");
    gBase = envDir ? envDir : ".";
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif
    gNotesDir = gBase + "/notes";
    bool firstRun = !dirExists(gNotesDir);
    ensureDir(gNotesDir);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("NoteGrid", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    gR = shotPath.empty() ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!gR) gR = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!gR) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(gR, W, H);
    SDL_SetRenderDrawBlendMode(gR, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < F_COUNT; i++) {
        std::string p = std::string(kAssetDir) + kFonts[i].file;
        gFont[i] = TTF_OpenFont(p.c_str(), kFonts[i].size);
        if (!gFont[i]) { fprintf(stderr, "font %s: %s\n", p.c_str(), TTF_GetError()); return 1; }
        TTF_SetFontHinting(gFont[i], TTF_HINTING_LIGHT);
    }

    loadSettings();
#ifndef __SWITCH__
    if (demo) seedDemo();
#endif
    if (firstRun && !demo) seedWelcome();
    loadNotes();
    rebuildVisible();

    Uint32 last = SDL_GetTicks();
    int frame = 0, scriptPos = 0, scriptWait = 0, shotWait = 0;
    std::vector<std::string> toks;
    { std::string t; for (char c : keyScript + ",") { if (c == ',') { if (!t.empty()) toks.push_back(t); t.clear(); } else t += c; } }

    while (gRunning
#ifdef __SWITCH__
           && appletMainLoop()
#endif
    ) {
        Uint32 now = SDL_GetTicks();
        double dt = std::min(0.1, (now - last) / 1000.0);
        last = now;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) gRunning = false;
            else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) handleFinger(e);
#ifndef __SWITCH__
            else if (e.type == SDL_KEYDOWN) keyToPending(e.key.keysym.sym);
#endif
        }
#ifndef __SWITCH__
        if (!toks.empty() && ++scriptWait >= 6) {
            scriptWait = 0;
            if (scriptPos < (int)toks.size()) tokenToPending(toks[scriptPos++]);
        }
#endif
        In in;
        pollInput(in, dt);
        if (in.plus) gRunning = false;
        if (gModal != M_NONE) updateModal(in);
        else if (gScreen == S_GRID) updateGrid(in);
        else updateView(in, dt);
        updateLongPress();

        if (gDirty) { layoutGrid(); ensureVisible(); }
        if (gScreen == S_VIEW && gVDirty) layoutView();

        float k = 1.f - expf(-(float)dt * 16.f);
        gScroll += (gScrollT - gScroll) * k;
        gVScroll += (gVScrollT - gVScroll) * k;
        if (fabsf(gScrollT - gScroll) < 0.3f) gScroll = gScrollT;
        if (fabsf(gVScrollT - gVScroll) < 0.3f) gVScroll = gVScrollT;
        if (gToastT > 0) gToastT -= dt;

        SDL_SetRenderDrawColor(gR, C_BG.r, C_BG.g, C_BG.b, 255);
        SDL_RenderClear(gR);
        if (gScreen == S_GRID) drawGrid(); else drawView();
        drawToast();
        drawModal();
        drawBottomHints();

        frame++;
#ifndef __SWITCH__
        if (!shotPath.empty() && scriptPos >= (int)toks.size() && ++shotWait > 30 && frame > 20) {
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(gR, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, shotPath.c_str());
            SDL_FreeSurface(s);
            break;
        }
#endif
        SDL_RenderPresent(gR);
    }

    for (auto& kv : gText) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    for (auto& kv : gMasks) SDL_DestroyTexture(kv.second);
    for (int i = 0; i < F_COUNT; i++) if (gFont[i]) TTF_CloseFont(gFont[i]);
    SDL_DestroyRenderer(gR);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
#ifdef __SWITCH__
    timeExit();
    romfsExit();
#endif
    return 0;
}
