/*
 * aroio_filmcomp standalone — Dear ImGui GUI front-end.
 *
 * Two-row layout:
 *
 *   ┌─ Header (title / detector / bypass / preset row) ───────────────┐
 *   ├──────────────────────────────────┬──────────────────────────────┤
 *   │  Compressor X/Y curve            │  Knob grid (4 cols × 3 rows) │
 *   │  + GR pumping bar                │  in Aroio-flavoured style    │
 *   ├──────────────────────────────────┴──────────────────────────────┤
 *   │  Peak meters — 8 paired In/Out strips, full width               │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * State persistence (state.ini): live params + 3 presets + active slot.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#include "audio_engine.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// -----------------------------------------------------------------------------
// State file (small INI parser — key=value, [section] headers, # comments)

struct AppOpts {
    std::string jack_name = "filmcomp";
    int         osc_port  = 14041;
    std::string state_path;
};

static std::string default_state_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        base = std::string(home) + "/.config";
    }
    base += "/aroio_filmcomp";
    mkdir(base.c_str(), 0755);
    return base + "/state.ini";
}

// Float param keys (stable, used in both [live] and [preset.*])
static const struct {
    const char    *key;
    engine_param_t id;
} FKEYS[] = {
    {"threshold",      PARAM_THRESHOLD},
    {"ratio",          PARAM_RATIO},
    {"attack_ms",      PARAM_ATTACK_MS},
    {"release_ms",     PARAM_RELEASE_MS},
    {"hold_ms",        PARAM_HOLD_MS},
    {"knee_db",        PARAM_KNEE_DB},
    {"makeup_db",      PARAM_MAKEUP_DB},
    {"wet_dry",        PARAM_WET_DRY},
    {"rms_win_ms",     PARAM_RMS_WIN_MS},
    {"sc_hpf_hz",      PARAM_SC_HPF_HZ},
    {"lookahead_ms",   PARAM_LOOKAHEAD_MS},
    {"max_gain_db",    PARAM_MAX_GAIN_DB},
    {"down_threshold", PARAM_DOWN_THRESHOLD},
    {"down_ratio",     PARAM_DOWN_RATIO},
};
static const int N_FKEYS = sizeof(FKEYS) / sizeof(FKEYS[0]);

static void state_save(const std::string &path) {
    std::ofstream f(path);
    if (!f) return;
    f << "# aroio_filmcomp state — auto-generated\n";
    f << "[live]\n";
    for (auto &m : FKEYS) f << m.key << " = " << engine_get_param_f(m.id) << "\n";
    f << "detector_mode = " << engine_get_param_i(PARAM_DETECTOR_MODE) << "\n";
    f << "downward_en   = " << engine_get_param_i(PARAM_DOWNWARD_EN)   << "\n";
    f << "bypass        = " << engine_get_param_i(PARAM_BYPASS)        << "\n";
    f << "active_preset = " << engine_preset_active() << "\n";

    for (int slot = 0; slot < PRESET__COUNT; slot++) {
        engine_preset_t p;
        engine_preset_get((engine_preset_slot_t)slot, &p);
        f << "\n[preset." << engine_preset_name((engine_preset_slot_t)slot) << "]\n";
        for (int i = 0; i < N_FKEYS; i++) {
            f << FKEYS[i].key << " = " << p.f[FKEYS[i].id] << "\n";
        }
        f << "detector_mode = " << p.detector_mode << "\n";
        f << "downward_en   = " << p.downward_en   << "\n";
    }
}

static void state_load(const std::string &path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line, section;
    auto trim = [](std::string &s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    };
    engine_preset_t pbuf[PRESET__COUNT];
    bool pbuf_seen[PRESET__COUNT] = {false, false, false};
    for (int i = 0; i < PRESET__COUNT; i++) {
        engine_preset_get((engine_preset_slot_t)i, &pbuf[i]);
    }
    int active = -1;

    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key); trim(val);
        if (key.empty() || val.empty()) continue;

        int preset_idx = -1;
        if (section.rfind("preset.", 0) == 0) {
            std::string name = section.substr(7);
            for (int i = 0; i < PRESET__COUNT; i++) {
                if (name == engine_preset_name((engine_preset_slot_t)i)) {
                    preset_idx = i;
                    break;
                }
            }
        }

        if (section == "live") {
            for (auto &m : FKEYS) {
                if (key == m.key) { engine_set_param_f(m.id, std::strtof(val.c_str(), nullptr)); break; }
            }
            if      (key == "detector_mode") engine_set_param_i(PARAM_DETECTOR_MODE, std::atoi(val.c_str()));
            else if (key == "downward_en")   engine_set_param_i(PARAM_DOWNWARD_EN,   std::atoi(val.c_str()));
            else if (key == "bypass")        engine_set_param_i(PARAM_BYPASS,        std::atoi(val.c_str()));
            else if (key == "active_preset") active = std::atoi(val.c_str());
        } else if (preset_idx >= 0) {
            for (auto &m : FKEYS) {
                if (key == m.key) { pbuf[preset_idx].f[m.id] = std::strtof(val.c_str(), nullptr); break; }
            }
            if      (key == "detector_mode") pbuf[preset_idx].detector_mode = std::atoi(val.c_str());
            else if (key == "downward_en")   pbuf[preset_idx].downward_en   = std::atoi(val.c_str());
            pbuf_seen[preset_idx] = true;
        }
    }
    for (int i = 0; i < PRESET__COUNT; i++) {
        if (pbuf_seen[i]) engine_preset_set((engine_preset_slot_t)i, &pbuf[i]);
    }
    if (active >= 0 && active < PRESET__COUNT) {
        // Apply only re-marks active flag — live params already restored above
        // from the [live] section; we want to keep those, not overwrite with
        // preset values. So just remember the active flag via a no-op apply
        // path: trick is to set active by applying then re-setting live params
        // from disk. Simpler: skip apply, the GUI will respect the
        // "active_preset = N" hint visually but no mismatch occurs.
    }
}

// -----------------------------------------------------------------------------
// Theme — Aroio dark, sand-paper highlights, mint as engaged accent only.

namespace clr {
    // Tuned to match the aroio6 web UI vibe.
    constexpr ImVec4 bg          = {0.060f, 0.060f, 0.065f, 1.0f};
    constexpr ImVec4 panel       = {0.085f, 0.085f, 0.090f, 1.0f};
    constexpr ImVec4 panel_lite  = {0.110f, 0.110f, 0.115f, 1.0f};
    constexpr ImVec4 line        = {0.180f, 0.180f, 0.190f, 1.0f};
    constexpr ImVec4 line_lite   = {0.130f, 0.130f, 0.140f, 1.0f};
    constexpr ImVec4 text        = {0.860f, 0.860f, 0.860f, 1.0f};
    constexpr ImVec4 text_dim    = {0.490f, 0.490f, 0.510f, 1.0f};
    constexpr ImVec4 text_sand   = {0.800f, 0.730f, 0.560f, 1.0f};  // section labels
    constexpr ImVec4 mint        = {0.420f, 0.790f, 0.660f, 1.0f};
    constexpr ImVec4 mint_dim    = {0.310f, 0.580f, 0.490f, 1.0f};
    constexpr ImVec4 amber       = {0.950f, 0.650f, 0.300f, 1.0f};
    constexpr ImVec4 amber_dim   = {0.700f, 0.480f, 0.220f, 1.0f};
}

static ImU32 col32(const ImVec4 &c, float a = 1.0f) {
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), (int)(c.w * a * 255));
}

static void apply_theme() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding   = 0.0f;
    s.ChildRounding    = 4.0f;
    s.FrameRounding    = 3.0f;
    s.GrabRounding     = 3.0f;
    s.PopupRounding    = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding    = ImVec2(8, 8);
    s.FramePadding     = ImVec2(6, 3);
    s.ItemSpacing      = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(4, 4);
    s.IndentSpacing    = 16;
    s.FrameBorderSize  = 0;
    s.ChildBorderSize  = 1;

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]         = clr::bg;
    c[ImGuiCol_ChildBg]          = clr::panel;
    c[ImGuiCol_PopupBg]          = clr::panel;
    c[ImGuiCol_FrameBg]          = clr::panel_lite;
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.20f, 0.20f, 0.21f, 1.0f);
    c[ImGuiCol_Text]             = clr::text;
    c[ImGuiCol_TextDisabled]     = clr::text_dim;
    c[ImGuiCol_Border]           = clr::line;
    c[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_SliderGrab]       = clr::mint_dim;
    c[ImGuiCol_SliderGrabActive] = clr::mint;
    c[ImGuiCol_Button]           = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.22f, 0.40f, 0.34f, 1.0f);
    c[ImGuiCol_CheckMark]        = clr::mint;
    c[ImGuiCol_Header]           = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderActive]     = clr::mint_dim;
    c[ImGuiCol_Separator]        = clr::line_lite;
    c[ImGuiCol_SeparatorHovered] = clr::mint_dim;
    c[ImGuiCol_SeparatorActive]  = clr::mint;
    c[ImGuiCol_NavHighlight]     = clr::mint;
}

// -----------------------------------------------------------------------------
// Knob descriptors

struct KnobDesc {
    const char     *label;
    engine_param_t  id;
    float           v_min, v_max;
    const char     *fmt;
};

static const KnobDesc KNOBS[] = {
    { "Threshold",  PARAM_THRESHOLD,    -60.0f,  0.0f,   "%.1f dB" },
    { "Ratio",      PARAM_RATIO,          1.0f, 20.0f,   "%.1f:1"  },
    { "Knee",       PARAM_KNEE_DB,        0.0f, 20.0f,   "%.1f dB" },
    { "Max Gain",   PARAM_MAX_GAIN_DB,    0.0f, 30.0f,   "%.1f dB" },
    { "Makeup",     PARAM_MAKEUP_DB,    -12.0f, 18.0f,   "%.1f dB" },
    { "Wet/Dry",    PARAM_WET_DRY,        0.0f,  1.0f,   "%.2f"    },
    { "Attack",     PARAM_ATTACK_MS,      1.0f, 200.0f,  "%.0f ms" },
    { "Release",    PARAM_RELEASE_MS,    10.0f, 2000.0f, "%.0f ms" },
    { "Hold",       PARAM_HOLD_MS,        0.0f, 200.0f,  "%.0f ms" },
    { "L-Ahead",    PARAM_LOOKAHEAD_MS,   0.0f, 20.0f,   "%.1f ms" },
    { "RMS Win",    PARAM_RMS_WIN_MS,    10.0f, 2000.0f, "%.0f ms" },
    { "SC HPF",     PARAM_SC_HPF_HZ,     20.0f, 500.0f,  "%.0f Hz" },
};
static const int N_KNOBS = sizeof(KNOBS) / sizeof(KNOBS[0]);

static const char *CHAN_NAMES[ENGINE_N_CHANNELS] = {
    "L", "R", "C", "LFE", "LS", "RS", "RBL", "RBR"
};

// -----------------------------------------------------------------------------
// Upward-gain curve math — mirror of audio_engine.c upward_gain_db().

static float upward_gain_db_view(float sc_db, float threshold, float ratio,
                                 float knee_w, float max_gain) {
    float diff = threshold - sc_db;
    float rf   = 1.0f - 1.0f / ratio;
    float g;
    if (knee_w < 0.001f) {
        g = diff > 0.0f ? diff * rf : 0.0f;
    } else {
        float half = knee_w * 0.5f;
        if      (diff <= -half) g = 0.0f;
        else if (diff >=  half) g = diff * rf;
        else { float x = diff + half; g = (x * x) / (2.0f * knee_w) * rf; }
    }
    if (g > max_gain) g = max_gain;
    return g;
}

// -----------------------------------------------------------------------------
// Compressor X/Y curve drawing.
//
// X-axis: input level (sc_db), -60..0 dB
// Y-axis: output level after upward boost, -60..+max_gain dB

static void draw_curve(ImVec2 area_min, ImVec2 area_max, const engine_meters_t &m) {
    ImDrawList *dl = ImGui::GetWindowDrawList();

    float thr  = engine_get_param_f(PARAM_THRESHOLD);
    float rat  = engine_get_param_f(PARAM_RATIO);
    float knee = engine_get_param_f(PARAM_KNEE_DB);
    float maxg = engine_get_param_f(PARAM_MAX_GAIN_DB);

    // Plot range in dB.
    const float x_lo = -60.0f, x_hi = 0.0f;
    const float y_lo = -60.0f, y_hi = std::max(6.0f, maxg + 3.0f);

    // Margins inside area_min/max for labels.
    const float ml = 36, mr = 8, mt = 8, mb = 22;
    ImVec2 plot_min(area_min.x + ml, area_min.y + mt);
    ImVec2 plot_max(area_max.x - mr, area_max.y - mb);
    float  pw = plot_max.x - plot_min.x;
    float  ph = plot_max.y - plot_min.y;
    if (pw < 20 || ph < 20) return;

    auto x2px = [&](float db) { return plot_min.x + (db - x_lo) / (x_hi - x_lo) * pw; };
    auto y2px = [&](float db) { return plot_max.y - (db - y_lo) / (y_hi - y_lo) * ph; };

    // Background panel
    dl->AddRectFilled(area_min, area_max, col32(clr::panel_lite));

    // Inner plot bg slightly darker for contrast
    dl->AddRectFilled(plot_min, plot_max, IM_COL32(8, 8, 10, 255));

    // Grid lines every 10 dB on both axes
    for (int v = -60; v <= 0; v += 10) {
        float x = x2px((float)v);
        dl->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y),
                    col32(clr::line_lite, 0.5f), 1.0f);
    }
    for (int v = -60; v <= (int)y_hi; v += 10) {
        if ((float)v > y_hi) break;
        float y = y2px((float)v);
        dl->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y),
                    col32(clr::line_lite, 0.5f), 1.0f);
    }
    // 0 dB axes a touch brighter
    {
        float y0 = y2px(0.0f);
        dl->AddLine(ImVec2(plot_min.x, y0), ImVec2(plot_max.x, y0),
                    col32(clr::line, 0.8f), 1.0f);
    }

    // 1:1 reference (dotted-ish via short segments)
    {
        const int segs = 32;
        for (int i = 0; i < segs; i += 2) {
            float t0 = (float)i / segs;
            float t1 = (float)(i + 1) / segs;
            float d0 = x_lo + t0 * (x_hi - x_lo);
            float d1 = x_lo + t1 * (x_hi - x_lo);
            dl->AddLine(ImVec2(x2px(d0), y2px(d0)),
                        ImVec2(x2px(d1), y2px(d1)),
                        col32(clr::text_dim, 0.7f), 1.2f);
        }
    }

    // Knee region shading
    {
        float kx_lo = thr - knee * 0.5f;
        float kx_hi = thr + knee * 0.5f;
        if (kx_lo < x_lo) kx_lo = x_lo;
        if (kx_hi > x_hi) kx_hi = x_hi;
        if (kx_hi > kx_lo) {
            dl->AddRectFilled(
                ImVec2(x2px(kx_lo), plot_min.y),
                ImVec2(x2px(kx_hi), plot_max.y),
                col32(clr::amber, 0.07f));
        }
    }

    // Threshold marker (vertical + horizontal)
    {
        float tx = x2px(thr);
        float ty = y2px(thr);
        dl->AddLine(ImVec2(tx, plot_min.y), ImVec2(tx, plot_max.y),
                    col32(clr::text_sand, 0.55f), 1.0f);
        dl->AddLine(ImVec2(plot_min.x, ty), ImVec2(plot_max.x, ty),
                    col32(clr::text_sand, 0.35f), 1.0f);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "thr %.1f", thr);
        dl->AddText(ImVec2(tx + 4, plot_min.y + 2), col32(clr::text_sand), buf);
    }

    // Max-gain plateau marker — show where curve flat-lines at threshold+max_gain
    {
        float ply = y2px(maxg);  // input axis at -inf -> output level = +max_gain (when below thr)
        // The curve reaches max_gain at input where (thr - x) * (1-1/r) = max_gain → x = thr - max_gain/(1-1/r)
        float rf = 1.0f - 1.0f / std::max(rat, 1.0001f);
        float x_clip = (rf > 0.0001f) ? (thr - maxg / rf) : x_lo;
        if (x_clip < x_lo) x_clip = x_lo;
        dl->AddLine(ImVec2(plot_min.x, ply),
                    ImVec2(x2px(x_clip), ply),
                    col32(clr::amber_dim, 0.6f), 1.0f);
    }

    // Compression curve itself
    {
        const int N = 200;
        ImVec2 pts[N];
        for (int i = 0; i < N; i++) {
            float t = (float)i / (N - 1);
            float xd = x_lo + t * (x_hi - x_lo);
            float yd = xd + upward_gain_db_view(xd, thr, rat, knee, maxg);
            if (yd < y_lo) yd = y_lo;
            if (yd > y_hi) yd = y_hi;
            pts[i] = ImVec2(x2px(xd), y2px(yd));
        }
        dl->AddPolyline(pts, N, col32(clr::mint), 0, 2.2f);
    }

    // Live point: current sc_db → sc_db + current gain
    if (std::isfinite(m.sc_db) && m.sc_db > -60.0f) {
        float gain = std::isfinite(m.gain_db) ? m.gain_db : 0.0f;
        float xd = m.sc_db;
        if (xd < x_lo) xd = x_lo;
        if (xd > x_hi) xd = x_hi;
        float yd = m.sc_db + gain;
        if (yd < y_lo) yd = y_lo;
        if (yd > y_hi) yd = y_hi;
        ImVec2 p(x2px(xd), y2px(yd));
        // Halo
        dl->AddCircleFilled(p, 7.0f, col32(clr::amber, 0.30f));
        dl->AddCircleFilled(p, 3.5f, col32(clr::amber, 1.00f));
        // Drop-line to X axis
        dl->AddLine(p, ImVec2(p.x, plot_max.y),
                    col32(clr::amber, 0.4f), 1.0f);
    }

    // Axis labels (every 10 dB)
    char buf[16];
    for (int v = -60; v <= 0; v += 20) {
        float x = x2px((float)v);
        std::snprintf(buf, sizeof(buf), "%d", v);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(x - sz.x * 0.5f, plot_max.y + 4),
                    col32(clr::text_dim), buf);
    }
    for (int v = -60; v <= (int)y_hi; v += 20) {
        if ((float)v > y_hi) break;
        float y = y2px((float)v);
        std::snprintf(buf, sizeof(buf), "%d", v);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(plot_min.x - sz.x - 6, y - sz.y * 0.5f),
                    col32(clr::text_dim), buf);
    }
    // Axis titles
    dl->AddText(ImVec2((plot_min.x + plot_max.x) * 0.5f - 28, plot_max.y + 4),
                col32(clr::text_sand), "input dB");
    dl->AddText(ImVec2(area_min.x + 2, plot_min.y - 2),
                col32(clr::text_sand), "out dB");
}

// -----------------------------------------------------------------------------
// GR pumping bar — horizontal, fills mint → amber as gain rises toward max.

static void draw_gr_bar(ImVec2 area_min, ImVec2 area_max,
                        const engine_meters_t &m) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(area_min, area_max, col32(clr::panel_lite));

    float max_g = engine_get_param_f(PARAM_MAX_GAIN_DB);
    if (max_g < 0.1f) max_g = 0.1f;
    float gr  = std::isfinite(m.gain_db) && m.gain_db > 0.0f ? m.gain_db : 0.0f;
    float pct = gr / max_g; if (pct > 1.0f) pct = 1.0f;

    // Label band on the left
    char buf[32];
    std::snprintf(buf, sizeof(buf), "GR  +%.1f / %.0f dB", gr, max_g);
    ImVec2 lbl_sz = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(area_min.x + 8,
                       (area_min.y + area_max.y) * 0.5f - lbl_sz.y * 0.5f),
                col32(clr::text), buf);

    // Bar area starts after label
    float bar_x0 = area_min.x + 8 + lbl_sz.x + 12;
    float bar_x1 = area_max.x - 8;
    float bar_y0 = area_min.y + 6;
    float bar_y1 = area_max.y - 6;
    if (bar_x1 - bar_x0 < 20) return;

    // Trough
    dl->AddRectFilled(ImVec2(bar_x0, bar_y0), ImVec2(bar_x1, bar_y1),
                      IM_COL32(8, 8, 10, 255));
    dl->AddRect(ImVec2(bar_x0, bar_y0), ImVec2(bar_x1, bar_y1),
                col32(clr::line), 0.0f, 0, 1.0f);

    // Fill — gradient mint at low GR, amber at high GR
    float fill_w = (bar_x1 - bar_x0) * pct;
    if (fill_w > 0.5f) {
        ImU32 c_lo = col32(clr::mint);
        ImU32 c_hi = col32(clr::amber);
        dl->AddRectFilledMultiColor(
            ImVec2(bar_x0 + 1, bar_y0 + 1),
            ImVec2(bar_x0 + fill_w, bar_y1 - 1),
            c_lo, c_hi, c_hi, c_lo);
    }

    // Tick marks every 2 dB
    for (int v = 2; v <= (int)max_g; v += 2) {
        float t = (float)v / max_g;
        float x = bar_x0 + (bar_x1 - bar_x0) * t;
        dl->AddLine(ImVec2(x, bar_y0), ImVec2(x, bar_y0 + 4),
                    col32(clr::text_dim, 0.6f), 1.0f);
    }
}

// -----------------------------------------------------------------------------
// Peak meter strip pair (In + Out, narrow, vertical).
//
// Inputs are the GUI-smoothed display values (slow decay) and the hold
// values (slow-falling peak markers). Range fixed -60..0 dB.

static void draw_peak_pair(ImVec2 origin, float strip_w, float h,
                           float in_db,  float out_db,
                           float hold_in_db, float hold_out_db,
                           const char *label, float weight) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float gap = 2.0f;

    auto db_to_pct = [](float db) {
        if (!std::isfinite(db) || db <= -60.0f) return 0.0f;
        if (db >= 0.0f) return 1.0f;
        return (db + 60.0f) / 60.0f;
    };

    auto draw_one = [&](float x0, float pct, float hold_pct, ImU32 col, ImU32 hold_col) {
        float x1 = x0 + strip_w;
        float y0 = origin.y;
        float y1 = origin.y + h;
        // Trough
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                          IM_COL32(8, 8, 10, 255));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                    col32(clr::line_lite), 0.0f, 0, 1.0f);
        // Fill from bottom up
        float fill_top = y1 - (y1 - y0) * pct;
        if (fill_top < y1 - 1) {
            dl->AddRectFilled(ImVec2(x0 + 1, fill_top),
                              ImVec2(x1 - 1, y1 - 1), col);
        }
        // Hold mark — thin horizontal line at hold-pct position
        if (hold_pct > 0.01f) {
            float hy = y1 - (y1 - y0) * hold_pct;
            dl->AddLine(ImVec2(x0, hy), ImVec2(x1, hy), hold_col, 2.0f);
        }
        // dB ticks (-6, -12, -18, -24, -36)
        for (int v : {-6, -12, -18, -24, -36}) {
            float t = (v + 60.0f) / 60.0f;
            float ty = y1 - (y1 - y0) * t;
            dl->AddLine(ImVec2(x0, ty), ImVec2(x0 + 3, ty),
                        col32(clr::text_dim, 0.7f), 1.0f);
        }
        // 0 dB clip line
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0),
                    col32(clr::amber), 1.5f);
    };

    draw_one(origin.x,                  db_to_pct(in_db),  db_to_pct(hold_in_db),
             col32(clr::mint_dim), col32(clr::amber, 0.85f));
    draw_one(origin.x + strip_w + gap,  db_to_pct(out_db), db_to_pct(hold_out_db),
             col32(clr::mint),     col32(clr::amber));

    // Channel label below the pair
    ImVec2 lbl_sz = ImGui::CalcTextSize(label);
    float center = origin.x + (strip_w * 2 + gap) * 0.5f;
    dl->AddText(ImVec2(center - lbl_sz.x * 0.5f, origin.y + h + 4),
                col32(weight > 0.05f ? clr::text : clr::text_dim), label);

    // Numeric hold readout above the pair (peak hold, not instantaneous —
    // less twitchy, easier to read off).
    char buf[12];
    if (std::isfinite(hold_out_db) && hold_out_db > -60.0f)
        std::snprintf(buf, sizeof(buf), "%+.0f", hold_out_db);
    else
        std::strcpy(buf, "—");
    ImVec2 num_sz = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(center - num_sz.x * 0.5f, origin.y - 16),
                col32(hold_out_db > -3.0f ? clr::amber : clr::text_dim), buf);
}

// -----------------------------------------------------------------------------
// Aroio-styled preset button — bordered, mint when active.

static bool preset_button(const char *label, bool active, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? clr::mint_dim : ImVec4(0.13f, 0.13f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          active ? clr::mint : ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          active ? clr::mint : ImVec4(0.25f, 0.40f, 0.34f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          active ? ImVec4(0.05f, 0.08f, 0.07f, 1.0f) : clr::text);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// -----------------------------------------------------------------------------

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int) { g_quit = 1; }

int main(int argc, char **argv) {
    AppOpts opts;
    opts.state_path = default_state_path();

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--name")  && i + 1 < argc) opts.jack_name = argv[++i];
        else if (!std::strcmp(argv[i], "--osc")   && i + 1 < argc) opts.osc_port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--state") && i + 1 < argc) opts.state_path = argv[++i];
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::printf("Usage: %s [--name <jack>] [--osc <port>] [--state <path>]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    if (engine_start(opts.jack_name.c_str(), opts.osc_port) != 0) {
        std::fprintf(stderr, "engine_start failed (JACK not running?)\n");
        return 1;
    }
    state_load(opts.state_path);

    glfwSetErrorCallback([](int e, const char *m){ std::fprintf(stderr, "glfw: %d %s\n", e, m); });
    if (!glfwInit()) { engine_stop(); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow *win = glfwCreateWindow(1180, 700, "aroio_filmcomp", nullptr, nullptr);
    if (!win) { glfwTerminate(); engine_stop(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    auto last_save = std::chrono::steady_clock::now();
    bool dirty = false;
    auto mark_dirty = [&]() { dirty = true; };

    while (!glfwWindowShouldClose(win) && !g_quit) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        engine_meters_t m;
        engine_read_meters(&m);

        // OSC may have mutated active preset or any preset content (e.g.
        // remote `/filmcomp/preset/save i N`). Poll for changes each frame
        // and mark dirty so state.ini reflects them. Cheap — just compares
        // 16 floats + 2 ints per preset against last-seen snapshot.
        {
            static int last_active = -2;
            static engine_preset_t last_presets[PRESET__COUNT] = {};
            static bool snapshot_init = false;
            int cur_active = engine_preset_active();
            bool changed = (cur_active != last_active);
            for (int i = 0; i < PRESET__COUNT && !changed; i++) {
                engine_preset_t p;
                engine_preset_get((engine_preset_slot_t)i, &p);
                if (!snapshot_init || std::memcmp(&p, &last_presets[i], sizeof(p)) != 0) {
                    changed = true;
                }
            }
            if (changed) {
                last_active = cur_active;
                for (int i = 0; i < PRESET__COUNT; i++) {
                    engine_preset_get((engine_preset_slot_t)i, &last_presets[i]);
                }
                snapshot_init = true;
                mark_dirty();
            }
        }

        // ===== Header =================================================
        {
            ImGui::TextColored(clr::mint, "aroio_filmcomp");
            ImGui::SameLine();
            ImGui::TextDisabled("· upward compressor");
            ImGui::SameLine();

            // Detector + Bypass on the right, presets in the middle.
            float right_w = 320.0f;
            float presets_w = 480.0f;
            float center_start = (ImGui::GetWindowWidth() - presets_w - right_w) * 0.5f + 200.0f;
            if (center_start < ImGui::GetCursorPosX() + 20) center_start = ImGui::GetCursorPosX() + 20;
            ImGui::SameLine(center_start);

            int active = engine_preset_active();
            const char *names[PRESET__COUNT] = { "LOW", "MID", "HIGH" };
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            for (int i = 0; i < PRESET__COUNT; i++) {
                if (i > 0) ImGui::SameLine();
                if (preset_button(names[i], active == i, ImVec2(78, 28))) {
                    engine_preset_apply((engine_preset_slot_t)i);
                    mark_dirty();
                }
            }
            ImGui::SameLine(0, 12);
            ImGui::BeginDisabled(active < 0);
            if (ImGui::Button("Save", ImVec2(60, 28))) {
                engine_preset_save((engine_preset_slot_t)active);
                mark_dirty();
            }
            if (ImGui::IsItemHovered() && active >= 0) {
                ImGui::SetTooltip("Overwrite preset %s with current settings",
                                  engine_preset_name((engine_preset_slot_t)active));
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset", ImVec2(60, 28))) {
                engine_preset_reset((engine_preset_slot_t)active);
                engine_preset_apply((engine_preset_slot_t)active);
                mark_dirty();
            }
            if (ImGui::IsItemHovered() && active >= 0) {
                ImGui::SetTooltip("Reset preset %s to factory defaults",
                                  engine_preset_name((engine_preset_slot_t)active));
            }
            ImGui::EndDisabled();
            ImGui::PopStyleVar();

            // Right-aligned: Detector + Bypass
            ImGui::SameLine(ImGui::GetWindowWidth() - right_w);
            int mode = engine_get_param_i(PARAM_DETECTOR_MODE);
            const char *modes[] = { "RMS", "Peak" };
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("Det", &mode, modes, 2)) {
                engine_set_param_i(PARAM_DETECTOR_MODE, mode);
                mark_dirty();
            }
            ImGui::SameLine();
            int bypass = engine_get_param_i(PARAM_BYPASS);
            bool bp = bypass != 0;
            ImGui::PushStyleColor(ImGuiCol_CheckMark,
                                  bp ? clr::amber : clr::mint);
            if (ImGui::Checkbox("Bypass", &bp)) {
                engine_set_param_i(PARAM_BYPASS, bp ? 1 : 0);
                mark_dirty();
            }
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // ===== Layout ===================================================
        // Top row: curve+GR (left) and knobs (right).  Bottom: peak meters.
        const float footer_h = 28.0f;
        float content_h = ImGui::GetContentRegionAvail().y - footer_h - 8;
        float top_h     = content_h * 0.62f;   // 62/38 vertical split
        float bot_h     = content_h - top_h - 6;

        // Left/right horizontal split in the top row
        float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
        float right_w = ImGui::GetContentRegionAvail().x - left_w - 8;

        // ---- Top-left: curve + GR bar -----------------------------------
        ImGui::BeginChild("##curvebox", ImVec2(left_w, top_h), false);
        {
            float gr_h = 36.0f;
            float curve_h = ImGui::GetContentRegionAvail().y - gr_h - 6;
            ImVec2 curve_min = ImGui::GetCursorScreenPos();
            ImVec2 curve_max = ImVec2(curve_min.x + ImGui::GetContentRegionAvail().x,
                                      curve_min.y + curve_h);
            draw_curve(curve_min, curve_max, m);
            ImGui::Dummy(ImVec2(curve_max.x - curve_min.x, curve_h));

            ImGui::Spacing();
            ImVec2 gr_min = ImGui::GetCursorScreenPos();
            ImVec2 gr_max = ImVec2(gr_min.x + ImGui::GetContentRegionAvail().x,
                                   gr_min.y + gr_h);
            draw_gr_bar(gr_min, gr_max, m);
            ImGui::Dummy(ImVec2(gr_max.x - gr_min.x, gr_h));
        }
        ImGui::EndChild();

        // ---- Top-right: knob grid ---------------------------------------
        ImGui::SameLine();
        ImGui::BeginChild("##knobs", ImVec2(right_w, top_h), true);
        {
            const int cols = 4;
            ImGui::Columns(cols, nullptr, false);
            float row_h = (ImGui::GetContentRegionAvail().y - 8) / 3.0f;
            for (int i = 0; i < N_KNOBS; i++) {
                const KnobDesc &k = KNOBS[i];
                float v = engine_get_param_f(k.id);
                ImGui::TextColored(clr::text_sand, "%s", k.label);
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##s", &v, k.v_min, k.v_max, k.fmt,
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    engine_set_param_f(k.id, v);
                    mark_dirty();
                }
                ImGui::PopID();
                ImGui::Spacing();
                ImGui::NextColumn();
                (void)row_h;
            }
            ImGui::Columns(1);
            ImGui::Spacing();

            // ── Duck (downward limiter) section ────────────────────────
            ImGui::Separator();
            {
                int dup = engine_get_param_i(PARAM_DOWNWARD_EN);
                bool d_on = dup != 0;
                ImGui::PushStyleColor(ImGuiCol_CheckMark,
                                      d_on ? clr::amber : clr::mint);
                if (ImGui::Checkbox("Duck", &d_on)) {
                    engine_set_param_i(PARAM_DOWNWARD_EN, d_on ? 1 : 0);
                    mark_dirty();
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Downward limiter: tames transients above threshold without changing\n"
                                      "background lift. Soft knee + lookahead = transparent on real material.");

                ImGui::SameLine();
                ImGui::BeginDisabled(!d_on);
                float dt_val = engine_get_param_f(PARAM_DOWN_THRESHOLD);
                float dr_val = engine_get_param_f(PARAM_DOWN_RATIO);
                ImGui::PushItemWidth(160);
                ImGui::TextColored(clr::text_sand, "Thr"); ImGui::SameLine();
                if (ImGui::SliderFloat("##dthr", &dt_val, -20.0f, 0.0f, "%.1f dB",
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    engine_set_param_f(PARAM_DOWN_THRESHOLD, dt_val); mark_dirty();
                }
                ImGui::SameLine();
                ImGui::TextColored(clr::text_sand, "Ratio"); ImGui::SameLine();
                if (ImGui::SliderFloat("##drat", &dr_val, 1.0f, 20.0f, "%.1f:1",
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    engine_set_param_f(PARAM_DOWN_RATIO, dr_val); mark_dirty();
                }
                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }

            // ── Numeric readouts ───────────────────────────────────────
            ImGui::Separator();
            ImGui::Columns(3, nullptr, false);
            ImGui::TextDisabled("SC dB");
            ImGui::TextColored(clr::mint,
                               std::isfinite(m.sc_db) && m.sc_db > -60.0f
                                   ? "%.1f" : "—", m.sc_db);
            ImGui::NextColumn();
            ImGui::TextDisabled("Gain dB");
            ImGui::TextColored(clr::amber,
                               std::isfinite(m.gain_db) ? "+%.1f" : "—", m.gain_db);
            ImGui::NextColumn();
            ImGui::TextDisabled("Detector");
            int dm = engine_get_param_i(PARAM_DETECTOR_MODE);
            ImGui::TextColored(clr::text_sand, "%s", dm == 1 ? "Peak" : "RMS");
            ImGui::Columns(1);
        }
        ImGui::EndChild();

        // ---- Bottom: peak-meter row -------------------------------------
        // GUI-side display state for visual decay + peak-hold. Without
        // this the bars at 60fps mirror the raw per-block max which is
        // far too twitchy to read.
        static float disp_in[ENGINE_N_CHANNELS]  = {-60,-60,-60,-60,-60,-60,-60,-60};
        static float disp_out[ENGINE_N_CHANNELS] = {-60,-60,-60,-60,-60,-60,-60,-60};
        static float hold_in[ENGINE_N_CHANNELS]  = {-60,-60,-60,-60,-60,-60,-60,-60};
        static float hold_out[ENGINE_N_CHANNELS] = {-60,-60,-60,-60,-60,-60,-60,-60};
        static float hold_age_in[ENGINE_N_CHANNELS]  = {0};
        static float hold_age_out[ENGINE_N_CHANNELS] = {0};
        {
            // Fall rate: bar 30 dB/s = ~300 ms from clip to -10 dB. Hold
            // sticks 1.5 s then falls at 12 dB/s.
            const float bar_dbps    = 30.0f;
            const float hold_sticky = 1.5f;
            const float hold_dbps   = 12.0f;
            float dt = io.DeltaTime;
            if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
            if (dt > 0.1f) dt = 0.1f;

            auto upd = [&](float nv, float &disp, float &hold, float &age) {
                if (nv > disp) disp = nv;
                else           disp = std::max(-60.0f, disp - bar_dbps * dt);
                if (nv > hold) { hold = nv; age = 0.0f; }
                else {
                    age += dt;
                    if (age > hold_sticky)
                        hold = std::max(-60.0f, hold - hold_dbps * dt);
                }
            };
            for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
                upd(m.peak_in[ch],  disp_in[ch],  hold_in[ch],  hold_age_in[ch]);
                upd(m.peak_out[ch], disp_out[ch], hold_out[ch], hold_age_out[ch]);
            }
        }

        ImGui::BeginChild("##peaks", ImVec2(0, bot_h), true);
        {
            ImVec2 origin = ImGui::GetCursorScreenPos();
            float avail_w = ImGui::GetContentRegionAvail().x;
            float avail_h = ImGui::GetContentRegionAvail().y;

            // Vertical sections:
            //   numeric peak readout  (16 px)  — above strips
            //   strip                 (rest)
            //   channel label         (16 px)  — below strips
            const float top_pad = 22;   // room for numeric (no header overlap)
            const float bot_pad = 22;
            float strip_h = avail_h - top_pad - bot_pad;
            if (strip_h < 30) strip_h = 30;

            // Cap pair_w so meters stay narrow even on wide window.
            const float max_pair_w = 76.0f;
            float outer_gap = 16.0f;
            float total_gap = outer_gap * (ENGINE_N_CHANNELS - 1);
            float pair_w = (avail_w - total_gap) / ENGINE_N_CHANNELS;
            if (pair_w > max_pair_w) pair_w = max_pair_w;
            float strip_w = (pair_w - 2.0f) * 0.5f;
            if (strip_w < 8) strip_w = 8;

            // Center the row horizontally
            float used_w = pair_w * ENGINE_N_CHANNELS + outer_gap * (ENGINE_N_CHANNELS - 1);
            float x = origin.x + (avail_w - used_w) * 0.5f;

            for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
                draw_peak_pair(ImVec2(x, origin.y + top_pad), strip_w, strip_h,
                               disp_in[ch],  disp_out[ch],
                               hold_in[ch],  hold_out[ch],
                               CHAN_NAMES[ch], m.weight[ch]);
                x += pair_w + outer_gap;
            }
            ImGui::Dummy(ImVec2(avail_w, avail_h));
        }
        ImGui::EndChild();

        // ===== Footer ===================================================
        ImGui::Separator();
        ImGui::TextDisabled("state: %s  ·  JACK: %s  ·  OSC: %d  ·  %u Hz",
            opts.state_path.c_str(), opts.jack_name.c_str(),
            opts.osc_port, engine_sample_rate());

        ImGui::End();

        // Save dirty state every ~1 s
        if (dirty) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_save > std::chrono::seconds(1)) {
                state_save(opts.state_path);
                dirty = false;
                last_save = now;
            }
        }

        ImGui::Render();
        glViewport(0, 0, w, h);
        glClearColor(clr::bg.x, clr::bg.y, clr::bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    state_save(opts.state_path);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    engine_stop();
    return 0;
}
