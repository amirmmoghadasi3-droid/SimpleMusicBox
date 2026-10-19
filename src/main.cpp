// Music Player - Dear ImGui (docking) + DirectX 11 + miniaudio
// Photoshop-style dark workspace: Tools | Canvas | Playlist + Properties | Timeline
#define IMGUI_DEFINE_MATH_OPERATORS
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace fs = std::filesystem;
#define ACC IM_COL32(20, 115, 230, 255)

// ------------------------------------------------------------ audio tap (scope + meters)
struct TapNode { ma_node_base base; };
static std::atomic<float> g_peakL{0.f}, g_peakR{0.f};
static float g_scope[2048];
static std::atomic<unsigned> g_scopeW{0};

static void TapProcess(ma_node*, const float** in, ma_uint32*, float** out, ma_uint32* frames) {
    const float* s = in[0]; float* d = out[0]; float pl = 0, pr = 0;
    unsigned w = g_scopeW.load(std::memory_order_relaxed);
    for (ma_uint32 i = 0; i < *frames; ++i) {
        float l = s[i * 2], r = s[i * 2 + 1];
        d[i * 2] = l; d[i * 2 + 1] = r;
        pl = std::max(pl, fabsf(l)); pr = std::max(pr, fabsf(r));
        g_scope[w++ & 2047] = (l + r) * 0.5f;
    }
    g_scopeW.store(w, std::memory_order_relaxed);
    g_peakL.store(pl); g_peakR.store(pr);
}

// ------------------------------------------------------------ player
struct Track { fs::path path; std::string name; double dur = 0; };

struct Player {
    ma_engine engine{}; TapNode tap{}; ma_sound sound{}; bool loaded = false;
    std::vector<Track> list; int cur = -1;
    bool playing = false, shuffle = false, repeat = false, muted = false;
    float volume = 0.8f, pan = 0.f, speed = 1.f;
    std::mt19937 rng{std::random_device{}()};

    bool init() {
        ma_engine_config c = ma_engine_config_init(); c.channels = 2;
        if (ma_engine_init(&c, &engine) != MA_SUCCESS) return false;
        static ma_node_vtable vt = {TapProcess, nullptr, 1, 1, 0};
        ma_uint32 ch[1] = {2};
        ma_node_config nc = ma_node_config_init(); nc.vtable = &vt; nc.pInputChannels = ch; nc.pOutputChannels = ch;
        if (ma_node_init(ma_engine_get_node_graph(&engine), &nc, nullptr, &tap.base) != MA_SUCCESS) return false;
        ma_node_attach_output_bus(&tap.base, 0, ma_engine_get_endpoint(&engine), 0);
        return true;
    }
    void unload() { if (loaded) { ma_sound_uninit(&sound); loaded = false; } }
    void shutdown() { unload(); ma_node_uninit(&tap.base, nullptr); ma_engine_uninit(&engine); }

    void add(const fs::path& q) {
        std::error_code ec;
        if (fs::is_directory(q, ec)) { for (auto& e : fs::recursive_directory_iterator(q, ec)) add(e.path()); return; }
        std::wstring x = q.extension().wstring(); for (auto& ch : x) ch = (wchar_t)towlower(ch);
        if (x != L".mp3" && x != L".wav" && x != L".flac") return;
        ma_decoder d; if (ma_decoder_init_file_w(q.c_str(), nullptr, &d) != MA_SUCCESS) return;
        ma_uint64 n = 0; ma_decoder_get_length_in_pcm_frames(&d, &n);
        Track t; t.path = q; t.name = q.stem().u8string(); t.dur = d.outputSampleRate ? n / (double)d.outputSampleRate : 0;
        ma_decoder_uninit(&d); list.push_back(t);
    }
    void stop() { unload(); playing = false; }
    void clear() { stop(); list.clear(); cur = -1; }
    void remove(int i) { if (i == cur) { stop(); cur = -1; } else if (i < cur) cur--; list.erase(list.begin() + i); }
    void play(int i) {
        if (i < 0 || i >= (int)list.size()) return;
        unload();
        if (ma_sound_init_from_file_w(&engine, list[i].path.c_str(), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &sound) != MA_SUCCESS) { playing = false; return; }
        loaded = true; cur = i;
        ma_node_attach_output_bus(&sound, 0, &tap.base, 0);
        ma_sound_start(&sound); playing = true;
    }
    void toggle() {
        if (!loaded) { play(cur >= 0 ? cur : 0); return; }
        if (playing) ma_sound_stop(&sound); else ma_sound_start(&sound);
        playing = !playing;
    }
    void next(bool automatic) {
        if (list.empty()) return;
        int n = cur + 1;
        if (shuffle && list.size() > 1) do n = (int)(rng() % list.size()); while (n == cur);
        if (n >= (int)list.size()) { if (repeat || !automatic) n = 0; else { stop(); return; } }
        play(n);
    }
    void prev() { if (pos() > 3.0) { seek(0); return; } if (!list.empty()) play(cur > 0 ? cur - 1 : (int)list.size() - 1); }
    double pos() { float f = 0; if (loaded) ma_sound_get_cursor_in_seconds(&sound, &f); return f; }
    double len() {
        float f = 0; if (loaded) ma_sound_get_length_in_seconds(&sound, &f);
        return f > 0 ? f : (cur >= 0 ? list[cur].dur : 0);
    }
    void seek(double s) {
        if (!loaded) return;
        ma_uint32 sr = 0; ma_sound_get_data_format(&sound, nullptr, nullptr, &sr, nullptr, 0);
        ma_sound_seek_to_pcm_frame(&sound, (ma_uint64)(s * sr));
    }
    void update() {
        ma_engine_set_volume(&engine, muted ? 0.f : volume * volume);
        if (!loaded) return;
        ma_sound_set_pan(&sound, pan); ma_sound_set_pitch(&sound, speed);
        if (playing && ma_sound_at_end(&sound)) next(true);
    }
};
static Player* g_pl;

// ------------------------------------------------------------ Win32 + D3D11 plumbing
static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_sc; static ID3D11RenderTargetView* g_rtv;

static void CreateRTV() { ID3D11Texture2D* b; g_sc->GetBuffer(0, IID_PPV_ARGS(&b)); g_dev->CreateRenderTargetView(b, nullptr, &g_rtv); b->Release(); }
static void ReleaseRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
static bool CreateDevice(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = h; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0}; D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lv, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx) != S_OK) return false;
    CreateRTV(); return true;
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) { ReleaseRTV(); g_sc->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); CreateRTV(); }
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)w; UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) { wchar_t b[MAX_PATH * 4]; DragQueryFileW(d, i, b, MAX_PATH * 4); g_pl->add(b); }
        DragFinish(d); return 0; }
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void OpenFiles(HWND owner, Player& p) {
    static wchar_t buf[65536]; buf[0] = 0;
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = owner; o.lpstrFile = buf; o.nMaxFile = 65536;
    o.lpstrFilter = L"Audio (*.mp3;*.wav;*.flac)\0*.mp3;*.wav;*.flac\0All files\0*.*\0";
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return;
    std::wstring dir = buf; wchar_t* q = buf + dir.size() + 1;
    if (!*q) p.add(dir); else for (; *q; q += wcslen(q) + 1) p.add(fs::path(dir) / q);
}

// ------------------------------------------------------------ theme (Photoshop-like greys, blue accent)
static void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle(); ImVec4* c = s.Colors;
    auto K = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); };
    c[ImGuiCol_Text] = K(212, 212, 212); c[ImGuiCol_TextDisabled] = K(120, 120, 120);
    c[ImGuiCol_WindowBg] = K(41, 41, 41); c[ImGuiCol_ChildBg] = K(35, 35, 35); c[ImGuiCol_PopupBg] = K(35, 35, 35);
    c[ImGuiCol_Border] = K(24, 24, 24); c[ImGuiCol_BorderShadow] = K(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = K(30, 30, 30); c[ImGuiCol_FrameBgHovered] = K(52, 52, 52); c[ImGuiCol_FrameBgActive] = K(60, 60, 60);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_MenuBarBg] = K(30, 30, 30);
    c[ImGuiCol_ScrollbarBg] = K(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = K(70, 70, 70);
    c[ImGuiCol_ScrollbarGrabHovered] = K(90, 90, 90); c[ImGuiCol_ScrollbarGrabActive] = K(110, 110, 110);
    c[ImGuiCol_CheckMark] = K(45, 140, 235); c[ImGuiCol_SliderGrab] = K(20, 115, 230); c[ImGuiCol_SliderGrabActive] = K(45, 140, 235);
    c[ImGuiCol_Button] = K(58, 58, 58); c[ImGuiCol_ButtonHovered] = K(72, 72, 72); c[ImGuiCol_ButtonActive] = K(50, 50, 50);
    c[ImGuiCol_Header] = K(20, 115, 230, 90); c[ImGuiCol_HeaderHovered] = K(20, 115, 230, 140); c[ImGuiCol_HeaderActive] = K(20, 115, 230);
    c[ImGuiCol_Separator] = K(24, 24, 24);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = K(30, 30, 30); c[ImGuiCol_TabHovered] = K(58, 58, 58);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = K(41, 41, 41);
    c[ImGuiCol_TabSelectedOverline] = K(20, 115, 230); c[ImGuiCol_TabDimmedSelectedOverline] = K(20, 115, 230, 90);
    c[ImGuiCol_DockingPreview] = K(20, 115, 230, 120); c[ImGuiCol_DockingEmptyBg] = K(24, 24, 24);
    c[ImGuiCol_TableHeaderBg] = K(30, 30, 30); c[ImGuiCol_TableBorderStrong] = K(24, 24, 24); c[ImGuiCol_TableBorderLight] = K(46, 46, 46);
    c[ImGuiCol_TableRowBg] = K(0, 0, 0, 0); c[ImGuiCol_TableRowBgAlt] = K(255, 255, 255, 8);
    c[ImGuiCol_TextSelectedBg] = K(20, 115, 230, 110);
    s.WindowRounding = 0; s.ChildRounding = 2; s.FrameRounding = 3; s.PopupRounding = 3; s.GrabRounding = 3; s.ScrollbarRounding = 6; s.TabRounding = 0;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.WindowPadding = {10, 10}; s.FramePadding = {8, 5}; s.ItemSpacing = {8, 7}; s.ScrollbarSize = 12;
}

// ------------------------------------------------------------ custom widgets
enum Icon { I_PLAY, I_PAUSE, I_STOP, I_PREV, I_NEXT };
static bool IconBtn(const char* id, Icon ic, ImVec2 sz, bool accent = false) {
    ImVec2 p = ImGui::GetCursorScreenPos(); bool clk = ImGui::InvisibleButton(id, sz);
    bool h = ImGui::IsItemHovered(), a = ImGui::IsItemActive(); ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 bg = accent ? (a ? IM_COL32(10, 90, 200, 255) : h ? IM_COL32(45, 140, 235, 255) : ACC)
                      : (a ? IM_COL32(50, 50, 50, 255) : h ? IM_COL32(72, 72, 72, 255) : IM_COL32(58, 58, 58, 255));
    d->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    ImVec2 c = p + sz * 0.5f; float s = sz.y * 0.2f; ImU32 w = IM_COL32(235, 235, 235, 255);
    switch (ic) {
    case I_PLAY:  d->AddTriangleFilled({c.x - s * .7f, c.y - s}, {c.x - s * .7f, c.y + s}, {c.x + s, c.y}, w); break;
    case I_PAUSE: d->AddRectFilled({c.x - s * .8f, c.y - s}, {c.x - s * .2f, c.y + s}, w); d->AddRectFilled({c.x + s * .2f, c.y - s}, {c.x + s * .8f, c.y + s}, w); break;
    case I_STOP:  d->AddRectFilled({c.x - s * .8f, c.y - s * .8f}, {c.x + s * .8f, c.y + s * .8f}, w, 2); break;
    case I_PREV:  d->AddRectFilled({c.x - s, c.y - s}, {c.x - s * .6f, c.y + s}, w); d->AddTriangleFilled({c.x + s, c.y - s}, {c.x + s, c.y + s}, {c.x - s * .5f, c.y}, w); break;
    case I_NEXT:  d->AddRectFilled({c.x + s * .6f, c.y - s}, {c.x + s, c.y + s}, w); d->AddTriangleFilled({c.x - s, c.y - s}, {c.x - s, c.y + s}, {c.x + s * .5f, c.y}, w); break;
    }
    return clk;
}
// Returns true on release; *f holds the dragged fraction on that frame.
static bool SeekBar(const char* id, float* f, float w) {
    static float drag; const float H = 18; ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, H}); bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    if (act) { drag = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.f, 1.f); *f = drag; }
    bool rel = ImGui::IsItemDeactivated(); if (rel) *f = drag;
    ImDrawList* d = ImGui::GetWindowDrawList(); float y = p.y + H * 0.5f;
    d->AddRectFilled({p.x, y - 2}, {p.x + w, y + 2}, IM_COL32(24, 24, 24, 255), 2);
    d->AddRectFilled({p.x, y - 2}, {p.x + w * *f, y + 2}, ACC, 2);
    if (act || hov) d->AddCircleFilled({p.x + w * *f, y}, 6, IM_COL32(235, 235, 235, 255));
    return rel;
}
static void Toggle(const char* l, bool* v, ImVec2 sz, const char* tip) {
    bool on = *v; if (on) { ImGui::PushStyleColor(ImGuiCol_Button, ACC); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 140, 235, 255)); }
    if (ImGui::Button(l, sz)) *v = !*v;
    if (on) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
}
static std::string T(double s) { int t = (int)s; char b[16]; snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60); return b; }

// ------------------------------------------------------------ panels
static void DrawCanvas(Player& p) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 o = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail(), e = o + sz;
    dl->AddRectFilled(o, e, IM_COL32(26, 26, 26, 255));
    for (float x = 0; x < sz.x; x += 32) dl->AddLine({o.x + x, o.y}, {o.x + x, e.y}, IM_COL32(255, 255, 255, 8));
    for (float y = 0; y < sz.y; y += 32) dl->AddLine({o.x, o.y + y}, {e.x, o.y + y}, IM_COL32(255, 255, 255, 8));
    float mid = o.y + sz.y * 0.5f, amp = sz.y * 0.3f; dl->AddLine({o.x, mid}, {e.x, mid}, IM_COL32(255, 255, 255, 30));
    const int N = 1024; static ImVec2 pts[N]; unsigned w = g_scopeW.load();
    for (int i = 0; i < N; i++) { float s = p.playing ? g_scope[(w - N + i) & 2047] : 0.f; pts[i] = {o.x + sz.x * i / (N - 1), mid - s * amp}; }
    dl->AddPolyline(pts, N, IM_COL32(45, 140, 235, 255), 0, 2.f);
    static float ml, mr; ml = std::max(p.playing ? g_peakL.load() : 0.f, ml * 0.92f); mr = std::max(p.playing ? g_peakR.load() : 0.f, mr * 0.92f);
    float bw = 10, bh = sz.y * 0.5f, yb = e.y - 90;
    auto bar = [&](float x, float v) {
        dl->AddRectFilled({x, yb - bh}, {x + bw, yb}, IM_COL32(20, 20, 20, 255), 2);
        dl->AddRectFilled({x, yb - bh * std::min(v, 1.f)}, {x + bw, yb}, v > 0.9f ? IM_COL32(235, 70, 60, 255) : ACC, 2);
    };
    bar(e.x - 42, ml); bar(e.x - 26, mr);
    if (p.cur >= 0) {
        dl->AddText(ImGui::GetFont(), 26.f, {o.x + 24, e.y - 70}, IM_COL32(230, 230, 230, 255), p.list[p.cur].name.c_str());
        dl->AddText({o.x + 24, e.y - 36}, IM_COL32(140, 140, 140, 255), p.playing ? "Playing" : "Paused");
    } else {
        const char* m = "Drop audio files here, or choose Open in the toolbox";
        ImVec2 ts = ImGui::CalcTextSize(m); dl->AddText({o.x + (sz.x - ts.x) * 0.5f, mid + 30}, IM_COL32(140, 140, 140, 255), m);
    }
    ImGui::Dummy(sz);
}

static void DrawPlaylist(Player& p, HWND hwnd) {
    if (ImGui::Button("Add files")) OpenFiles(hwnd, p);
    ImGui::SameLine(); if (ImGui::Button("Clear")) p.clear();
    int rm = -1;
    if (ImGui::BeginTable("pl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)p.list.size(); i++) {
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::PushID(i);
            char num[8]; snprintf(num, sizeof num, i == p.cur ? ">" : "%d", i + 1);
            if (ImGui::Selectable(num, i == p.cur, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) p.play(i);
            if (ImGui::BeginPopupContextItem("ctx")) { if (ImGui::MenuItem("Play")) p.play(i); if (ImGui::MenuItem("Remove")) rm = i; ImGui::EndPopup(); }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.list[i].name.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", T(p.list[i].dur).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (rm >= 0) p.remove(rm);
}

static void DrawProperties(Player& p) {
    if (ImGui::CollapsingHeader("Track", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (p.cur >= 0) {
            ImGui::TextWrapped("%s", p.list[p.cur].name.c_str());
            ImGui::TextDisabled("%s", p.list[p.cur].path.u8string().c_str());
            ma_uint32 ch = 0, sr = 0; if (p.loaded) ma_sound_get_data_format(&p.sound, nullptr, &ch, &sr, nullptr, 0);
            ImGui::Text("Length: %s", T(p.len()).c_str()); ImGui::Text("Sample rate: %u Hz", sr); ImGui::Text("Channels: %u", ch);
        } else ImGui::TextDisabled("No track loaded");
    }
    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
        float v = p.volume * 100; if (ImGui::SliderFloat("Volume", &v, 0, 100, "%.0f%%")) p.volume = v / 100;
        ImGui::SliderFloat("Pan", &p.pan, -1, 1, "%.2f"); ImGui::SliderFloat("Speed", &p.speed, 0.5f, 2.f, "%.2fx");
        if (ImGui::Button("Reset pan / speed")) { p.pan = 0; p.speed = 1; }
    }
}

static void DrawTimeline(Player& p) {
    ImVec2 bs(38, 38);
    if (IconBtn("prev", I_PREV, bs)) p.prev(); ImGui::SameLine();
    if (IconBtn("play", p.playing ? I_PAUSE : I_PLAY, bs, true)) p.toggle(); ImGui::SameLine();
    if (IconBtn("stop", I_STOP, bs)) p.stop(); ImGui::SameLine();
    if (IconBtn("next", I_NEXT, bs)) p.next(false); ImGui::SameLine();
    float x0 = ImGui::GetCursorPosX() + 8, y = ImGui::GetCursorPosY(); double pos = p.pos(), len = p.len();
    ImGui::SetCursorPosX(x0); ImGui::SetCursorPosY(y + 10); ImGui::Text("%s / %s", T(pos).c_str(), T(len).c_str());
    ImGui::SameLine(x0 + 112); ImGui::SetCursorPosY(y + 10);
    float frac = len > 0 ? (float)(pos / len) : 0.f;
    if (SeekBar("##seek", &frac, ImGui::GetContentRegionAvail().x - 140) && len > 0) p.seek(frac * len);
    ImGui::SameLine(); ImGui::SetCursorPosY(y + 7); ImGui::SetNextItemWidth(-FLT_MIN);
    float v = p.volume * 100; if (ImGui::SliderFloat("##vol", &v, 0, 100, "Vol %.0f%%")) p.volume = v / 100;
}

static void BuildLayout(ImGuiID root) {
    ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderRemoveNode(root); ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace); ImGui::DockBuilderSetNodeSize(root, ws);
    ImGuiID center = root, left, bottom, right, rtop, rbot;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 68.f / ws.x, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 96.f / ws.y, &bottom, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42f, &rbot, &rtop);
    ImGui::DockBuilderDockWindow("Tools", left); ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Playlist", rtop); ImGui::DockBuilderDockWindow("Properties", rbot);
    ImGui::DockBuilderDockWindow("Canvas", center); ImGui::DockBuilderFinish(root);
}

static void DrawUI(Player& p, bool* show, HWND hwnd, bool& rebuild) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open files...", "Ctrl+O")) OpenFiles(hwnd, p);
            if (ImGui::MenuItem("Clear playlist")) p.clear();
            ImGui::Separator(); if (ImGui::MenuItem("Exit", "Alt+F4")) PostQuitMessage(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Playback")) {
            if (ImGui::MenuItem(p.playing ? "Pause" : "Play", "Space")) p.toggle();
            if (ImGui::MenuItem("Stop")) p.stop();
            if (ImGui::MenuItem("Previous")) p.prev();
            if (ImGui::MenuItem("Next")) p.next(false);
            ImGui::Separator(); ImGui::MenuItem("Shuffle", nullptr, &p.shuffle); ImGui::MenuItem("Repeat playlist", nullptr, &p.repeat);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            const char* n[] = {"Tools", "Canvas", "Playlist", "Properties", "Timeline"};
            for (int i = 0; i < 5; i++) ImGui::MenuItem(n[i], nullptr, &show[i]);
            ImGui::Separator(); if (ImGui::MenuItem("Reset layout")) rebuild = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) p.toggle();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenFiles(hwnd, p);
    }
    const ImGuiID dock = ImHashStr("MusicPlayerDock");
    if (rebuild) { rebuild = false; BuildLayout(dock); }
    ImGui::DockSpaceOverViewport(dock, ImGui::GetMainViewport());

    if (show[0]) {
        if (ImGui::Begin("Tools", &show[0])) {
            ImVec2 b(-FLT_MIN, 44);
            if (ImGui::Button("Open", b)) OpenFiles(hwnd, p);
            Toggle("Shuf", &p.shuffle, b, "Shuffle"); Toggle("Loop", &p.repeat, b, "Repeat playlist"); Toggle("Mute", &p.muted, b, "Mute");
        }
        ImGui::End();
    }
    if (show[1]) { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0}); if (ImGui::Begin("Canvas", &show[1])) DrawCanvas(p); ImGui::End(); ImGui::PopStyleVar(); }
    if (show[2]) { if (ImGui::Begin("Playlist", &show[2])) DrawPlaylist(p, hwnd); ImGui::End(); }
    if (show[3]) { if (ImGui::Begin("Properties", &show[3])) DrawProperties(p); ImGui::End(); }
    if (show[4]) { if (ImGui::Begin("Timeline", &show[4])) DrawTimeline(p); ImGui::End(); }
}

// ------------------------------------------------------------ entry point
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hi, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"MusicPlayerWnd", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Music Player", WS_OVERLAPPEDWINDOW, 100, 100, 1400, 860, nullptr, nullptr, hi, nullptr);
    BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DragAcceptFiles(hwnd, TRUE);
    if (!CreateDevice(hwnd)) { MessageBoxW(nullptr, L"Could not create the Direct3D 11 device.", L"Music Player", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    bool rebuild = !fs::exists("imgui.ini");
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ApplyTheme();
    float sc = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    ImGui::GetStyle().ScaleAllSizes(sc);
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.f * sc);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);

    Player pl; g_pl = &pl;
    if (!pl.init()) MessageBoxW(hwnd, L"Audio engine failed to start. Playback is unavailable.", L"Music Player", MB_ICONWARNING);
    bool show[5] = {true, true, true, true, true};

    for (bool done = false; !done;) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(hwnd)) { Sleep(10); continue; }
        pl.update();
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawUI(pl, show, hwnd, rebuild);
        ImGui::Render();
        const float clr[4] = {0.11f, 0.11f, 0.11f, 1.f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }
    pl.shutdown();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    ReleaseRTV(); g_sc->Release(); g_ctx->Release(); g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, hi);
    return 0;
}
