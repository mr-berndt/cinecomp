/*
 * cinecomp standalone — Dear ImGui GUI front-end.
 *
 * 2x2 main layout (v2 architecture), matching the aroio6 web UI:
 *
 *   ┌─ Header (title / presets · detector / bypass) ───────────────┐
 *   ├──────────────────────────────────┬────────────────────────────┤
 *   │ Gain-vs-SC curve                 │ Combined meter block:      │
 *   │ (plateau shape, live SC marker)  │  GR-bar + SC/Gain readout  │
 *   │                                  │  + 8-ch in/out peak meters │
 *   ├──────────────────────────────────┼────────────────────────────┤
 *   │ Upward heading + 3×3 knob matrix │ Downward heading + Duck    │
 *   │   col1: Atmo (Lift/Thr/Knee)     │   3×2 grid (Thr/Ratio/Knee │
 *   │   col2: Dialog (Lift/Thr/Knee)   │   Attack/Release)          │
 *   │   col3: Attack/Release           │   + Global: Makeup/SC Decay│
 *   └──────────────────────────────────┴────────────────────────────┘
 *
 * State persistence (state.ini): live params + 3 factory presets +
 * active slot + named (user) presets + active named selection.
 */

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
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
    std::string jack_name = "cinecomp";
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
    base += "/cinecomp";
    mkdir(base.c_str(), 0755);
    return base + "/state.ini";
}

// Float param keys (stable, used in both [live] and [preset.*])
static const struct {
    const char    *key;
    engine_param_t id;
} FKEYS[] = {
    {"threshold",         PARAM_THRESHOLD},
    {"ratio",             PARAM_RATIO},
    {"attack_ms",         PARAM_ATTACK_MS},
    {"release_ms",        PARAM_RELEASE_MS},
    {"hold_ms",           PARAM_HOLD_MS},
    {"knee_db",           PARAM_KNEE_DB},
    {"makeup_db",         PARAM_MAKEUP_DB},
    {"wet_dry",           PARAM_WET_DRY},
    {"rms_win_ms",        PARAM_RMS_WIN_MS},
    {"sc_hpf_hz",         PARAM_SC_HPF_HZ},
    {"lookahead_ms",      PARAM_LOOKAHEAD_MS},
    {"max_gain_db",       PARAM_MAX_GAIN_DB},
    {"down_threshold",    PARAM_DOWN_THRESHOLD},
    {"down_ratio",        PARAM_DOWN_RATIO},
    {"atmo_threshold",    PARAM_ATMO_THRESHOLD},
    {"atmo_max_gain",     PARAM_ATMO_MAX_GAIN},
    {"atmo_knee",         PARAM_ATMO_KNEE},
    {"dialog_threshold",  PARAM_DIALOG_THRESHOLD},
    {"dialog_max_gain",   PARAM_DIALOG_MAX_GAIN},
    {"dialog_knee",       PARAM_DIALOG_KNEE},
    {"noise_floor_db",    PARAM_NOISE_FLOOR_DB},
    {"noise_knee_db",     PARAM_NOISE_KNEE_DB},
    {"upward_attack_ms",  PARAM_UPWARD_ATTACK_MS},
    {"upward_release_ms", PARAM_UPWARD_RELEASE_MS},
    {"duck_attack_ms",    PARAM_DUCK_ATTACK_MS},
    {"duck_release_ms",   PARAM_DUCK_RELEASE_MS},
};
static const int N_FKEYS = sizeof(FKEYS) / sizeof(FKEYS[0]);

static void state_save(const std::string &path) {
    std::ofstream f(path);
    if (!f) return;
    f << "# cinecomp state - auto-generated\n";
    f << "[live]\n";
    for (auto &m : FKEYS) f << m.key << " = " << engine_get_param_f(m.id) << "\n";
    f << "detector_mode     = " << engine_get_param_i(PARAM_DETECTOR_MODE)     << "\n";
    f << "downward_en       = " << engine_get_param_i(PARAM_DOWNWARD_EN)       << "\n";
    f << "bypass            = " << engine_get_param_i(PARAM_BYPASS)            << "\n";
    f << "architecture_mode = " << engine_get_param_i(PARAM_ARCHITECTURE_MODE) << "\n";
    f << "active_preset     = " << engine_preset_active() << "\n";
    f << "active_named      = " << engine_named_active()   << "\n";

    for (int slot = 0; slot < PRESET__COUNT; slot++) {
        engine_preset_t p;
        engine_preset_get((engine_preset_slot_t)slot, &p);
        f << "\n[preset." << engine_preset_name((engine_preset_slot_t)slot) << "]\n";
        for (int i = 0; i < N_FKEYS; i++) {
            f << FKEYS[i].key << " = " << p.f[FKEYS[i].id] << "\n";
        }
        f << "detector_mode     = " << p.detector_mode     << "\n";
        f << "downward_en       = " << p.downward_en       << "\n";
        f << "architecture_mode = " << p.architecture_mode << "\n";
    }

    // Named (user) presets — one [named.<NAME>] section each.
    int nn = engine_named_count();
    for (int k = 0; k < nn; k++) {
        const char *nm = engine_named_name(k);
        if (!nm || !*nm) continue;
        std::string name = nm;   // copy: engine_named_name() reuses a buffer
        engine_preset_t p;
        if (engine_named_get(name.c_str(), &p) != 0) continue;
        f << "\n[named." << name << "]\n";
        for (int i = 0; i < N_FKEYS; i++) {
            f << FKEYS[i].key << " = " << p.f[FKEYS[i].id] << "\n";
        }
        f << "detector_mode     = " << p.detector_mode     << "\n";
        f << "downward_en       = " << p.downward_en       << "\n";
        f << "architecture_mode = " << p.architecture_mode << "\n";
    }
}

// Do the live params still match the named preset that is selected? A preset
// is a starting point, not a cage: now that the selection survives a restart
// without being re-applied, the label would otherwise claim a state that the
// knobs no longer have.
static bool live_matches_named(const char *name) {
    engine_preset_t p;
    if (engine_named_get(name, &p) != 0) return true;
    for (int i = 0; i < N_FKEYS; i++) {
        float a = engine_get_param_f(FKEYS[i].id);
        float b = p.f[FKEYS[i].id];
        if (std::fabs(a - b) > 1e-4f * (1.0f + std::fabs(b))) return false;
    }
    return p.detector_mode     == engine_get_param_i(PARAM_DETECTOR_MODE)
        && p.downward_en       == engine_get_param_i(PARAM_DOWNWARD_EN)
        && p.architecture_mode == engine_get_param_i(PARAM_ARCHITECTURE_MODE);
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

    // Named-preset accumulation: a [named.<NAME>] section's keys are
    // collected into nbuf, then committed to the engine when the section
    // changes (or at EOF). The factory MID preset seeds nbuf so partially
    // written sections still get sane zonal defaults.
    std::string     active_named;
    std::string     named_cur;          // name of the section being read
    engine_preset_t nbuf{};
    bool            nbuf_active = false;
    auto flush_named = [&]() {
        if (nbuf_active && !named_cur.empty())
            engine_named_set(named_cur.c_str(), &nbuf);
        nbuf_active = false;
        named_cur.clear();
    };

    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            flush_named();
            section = line.substr(1, line.size() - 2);
            if (section.rfind("named.", 0) == 0) {
                named_cur = section.substr(6);
                engine_preset_get((engine_preset_slot_t)PRESET_MID, &nbuf);
                nbuf_active = true;
            }
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

        if (nbuf_active) {
            bool found_f = false;
            for (auto &m : FKEYS) {
                if (key == m.key) { nbuf.f[m.id] = std::strtof(val.c_str(), nullptr); found_f = true; break; }
            }
            if (found_f) continue;
            if      (key == "detector_mode")     nbuf.detector_mode     = std::atoi(val.c_str());
            else if (key == "downward_en")       nbuf.downward_en       = std::atoi(val.c_str());
            else if (key == "architecture_mode") nbuf.architecture_mode = std::atoi(val.c_str());
        } else if (section == "live") {
            bool found_f = false;
            for (auto &m : FKEYS) {
                if (key == m.key) { engine_set_param_f(m.id, std::strtof(val.c_str(), nullptr)); found_f = true; break; }
            }
            if (found_f) continue;
            if      (key == "detector_mode")     engine_set_param_i(PARAM_DETECTOR_MODE,     std::atoi(val.c_str()));
            else if (key == "downward_en")       engine_set_param_i(PARAM_DOWNWARD_EN,       std::atoi(val.c_str()));
            else if (key == "bypass")            engine_set_param_i(PARAM_BYPASS,            std::atoi(val.c_str()));
            else if (key == "architecture_mode") engine_set_param_i(PARAM_ARCHITECTURE_MODE, std::atoi(val.c_str()));
            else if (key == "active_named")      active_named = val;
            // active_preset is informational only — see comment below.
        } else if (preset_idx >= 0) {
            bool found_f = false;
            for (auto &m : FKEYS) {
                if (key == m.key) { pbuf[preset_idx].f[m.id] = std::strtof(val.c_str(), nullptr); found_f = true; break; }
            }
            if (found_f) { pbuf_seen[preset_idx] = true; continue; }
            if      (key == "detector_mode")     pbuf[preset_idx].detector_mode     = std::atoi(val.c_str());
            else if (key == "downward_en")       pbuf[preset_idx].downward_en       = std::atoi(val.c_str());
            else if (key == "architecture_mode") pbuf[preset_idx].architecture_mode = std::atoi(val.c_str());
            pbuf_seen[preset_idx] = true;
        }
    }
    flush_named();   // commit the trailing [named.*] section
    for (int i = 0; i < PRESET__COUNT; i++) {
        if (pbuf_seen[i]) engine_preset_set((engine_preset_slot_t)i, &pbuf[i]);
    }
    // We deliberately do NOT call engine_preset_apply() with active_preset
    // here. The [live] section already restored the user's live params; an
    // apply would overwrite them with the preset slot's snapshot, which is
    // wrong if the user had unsaved edits.
    //
    // active_named is remembered but NOT applied, for the same reason.
    //
    // It used to be applied, on the reasoning that "the live section was just
    // its snapshot anyway" - which stops being true the moment anyone turns a
    // knob after picking a preset. Those edits went into [live] on save and
    // were overwritten again on the next start, so the program always came up
    // with the same settings no matter what had been dialled in. Picking a
    // preset is something the user does; starting the program is not.
    if (!active_named.empty()) {
        engine_preset_t tmp;
        if (engine_named_get(active_named.c_str(), &tmp) == 0)
            engine_named_set_active(active_named.c_str());
        else
            engine_named_set_active("");    // preset is gone, selection with it
    }
}

// -----------------------------------------------------------------------------
// Theme — dark mode, amber as primary accent (zonal-arch / live signal),
// mint as secondary (engaged / on-state), sand for section headings.

namespace clr {
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
    constexpr ImVec4 dialog_blue = {0.380f, 0.580f, 0.880f, 1.0f};
    constexpr ImVec4 atmo_purple = {0.640f, 0.470f, 0.880f, 1.0f};
    constexpr ImVec4 duck_red    = {0.880f, 0.380f, 0.380f, 1.0f};
}

static ImU32 col32(const ImVec4 &c, float a = 1.0f) {
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), (int)(c.w * a * 255));
}

static void apply_theme() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding     = ImVec2(8, 8);
    s.FramePadding      = ImVec2(6, 3);
    s.ItemSpacing       = ImVec2(8, 5);
    s.ItemInnerSpacing  = ImVec2(4, 4);
    s.IndentSpacing     = 16;
    s.FrameBorderSize   = 0;
    s.ChildBorderSize   = 1;

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
    c[ImGuiCol_SliderGrab]       = clr::amber_dim;
    c[ImGuiCol_SliderGrabActive] = clr::amber;
    c[ImGuiCol_Button]           = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.32f, 0.22f, 0.10f, 1.0f);
    c[ImGuiCol_CheckMark]        = clr::amber;
    c[ImGuiCol_Header]           = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderActive]     = clr::amber_dim;
    c[ImGuiCol_Separator]        = clr::line_lite;
    c[ImGuiCol_SeparatorHovered] = clr::amber_dim;
    c[ImGuiCol_SeparatorActive]  = clr::amber;
    c[ImGuiCol_NavHighlight]     = clr::amber;
}

// -----------------------------------------------------------------------------
// Channel layout

static const char *CHAN_NAMES[ENGINE_N_CHANNELS] = {
    "L", "R", "C", "LFE", "LS", "RS", "RBL", "RBR"
};

// -----------------------------------------------------------------------------
// Math mirrors of audio_engine.c for the curve view.

static float smoothstep01_view(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
static float zone_active_below_view(float sc, float center, float width) {
    if (width < 0.001f) return sc <= center ? 1.0f : 0.0f;
    float half = width * 0.5f;
    return smoothstep01_view((center + half - sc) / width);
}
static float zone_active_above_view(float sc, float center, float width) {
    return 1.0f - zone_active_below_view(sc, center, width);
}
static float noise_floor_atten_view(float sc, float nf, float nk) {
    if (nk < 0.001f) return sc >= nf ? 1.0f : 0.0f;
    if (sc >= nf) return 1.0f;
    if (sc <= nf - nk) return 0.0f;
    float t = (sc - (nf - nk)) / nk;
    return t * t * (3.0f - 2.0f * t);
}

// Classic-mode upward gain (mirror of upward_gain_db in engine).
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
static float downward_gain_db_view(float sc_db, float threshold, float ratio,
                                   float knee_w) {
    float diff = sc_db - threshold;
    float rf   = 1.0f - 1.0f / ratio;
    if (knee_w < 0.001f) return diff > 0.0f ? -diff * rf : 0.0f;
    float half = knee_w * 0.5f;
    if (diff <= -half) return 0.0f;
    if (diff >=  half) return -diff * rf;
    float x = diff + half;
    return -(x * x) / (2.0f * knee_w) * rf;
}

// Compute the gain target for any architecture at SC `sc`. Returns
// {gain_up, gain_dn} so the curve can render upward and downward
// separately for clarity.
struct GainPair { float up; float dn; };
static GainPair gain_target_view(float sc, int arch_mode) {
    GainPair r{0, 0};
    float ratio = engine_get_param_f(PARAM_RATIO);
    bool  duck  = engine_get_param_i(PARAM_DOWNWARD_EN) != 0;
    float duck_thr   = engine_get_param_f(PARAM_DOWN_THRESHOLD);
    float duck_ratio = engine_get_param_f(PARAM_DOWN_RATIO);
    float duck_knee  = engine_get_param_f(PARAM_KNEE_DB);

    if (arch_mode == 0) {
        // Classic single stage
        float thr  = engine_get_param_f(PARAM_THRESHOLD);
        float knee = engine_get_param_f(PARAM_KNEE_DB);
        float maxg = engine_get_param_f(PARAM_MAX_GAIN_DB);
        r.up = upward_gain_db_view(sc, thr, ratio, knee, maxg);
    } else {
        float atmo_t = engine_get_param_f(PARAM_ATMO_THRESHOLD);
        float atmo_m = engine_get_param_f(PARAM_ATMO_MAX_GAIN);
        float atmo_k = engine_get_param_f(PARAM_ATMO_KNEE);
        float dial_t = engine_get_param_f(PARAM_DIALOG_THRESHOLD);
        float dial_m = engine_get_param_f(PARAM_DIALOG_MAX_GAIN);
        float dial_k = engine_get_param_f(PARAM_DIALOG_KNEE);
        float nf     = engine_get_param_f(PARAM_NOISE_FLOOR_DB);
        float nk     = engine_get_param_f(PARAM_NOISE_KNEE_DB);

        if (arch_mode == 2) {
            float w_atmo = zone_active_below_view(sc, atmo_t, atmo_k);
            float w_dial = zone_active_above_view(sc, atmo_t, atmo_k)
                         * zone_active_below_view(sc, dial_t, dial_k);
            float nw     = noise_floor_atten_view(sc, nf, nk);
            r.up = atmo_m * w_atmo * nw + dial_m * w_dial;
        } else {
            // v1 summed
            float atmo = upward_gain_db_view(sc, atmo_t, ratio, atmo_k, atmo_m);
            atmo *= noise_floor_atten_view(sc, nf, nk);
            float dial = upward_gain_db_view(sc, dial_t, ratio, dial_k, dial_m);
            r.up = atmo + dial;
        }
    }
    if (duck) r.dn = downward_gain_db_view(sc, duck_thr, duck_ratio, duck_knee);
    return r;
}

// -----------------------------------------------------------------------------
// Gain-vs-SC curve drawing.
//
// X-axis: side-chain SC in dB, range -80..0
// Y-axis: gain applied (upward positive, downward negative), -20..+30 dB

static void draw_curve(ImVec2 area_min, ImVec2 area_max,
                       const engine_meters_t &m, int arch_mode) {
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float x_lo = -80.0f, x_hi = 0.0f;
    const float y_lo = -20.0f, y_hi = 30.0f;

    const float ml = 38, mr = 8, mt = 18, mb = 22;
    ImVec2 plot_min(area_min.x + ml, area_min.y + mt);
    ImVec2 plot_max(area_max.x - mr, area_max.y - mb);
    float  pw = plot_max.x - plot_min.x;
    float  ph = plot_max.y - plot_min.y;
    if (pw < 20 || ph < 20) return;

    auto x2px = [&](float db) { return plot_min.x + (db - x_lo) / (x_hi - x_lo) * pw; };
    auto y2px = [&](float db) { return plot_max.y - (db - y_lo) / (y_hi - y_lo) * ph; };

    // Panel
    dl->AddRectFilled(area_min, area_max, col32(clr::panel_lite));
    // Inner plot bg
    dl->AddRectFilled(plot_min, plot_max, IM_COL32(8, 8, 10, 255));

    // Zone fills (v2 only) — visualize the plateau topology.
    if (arch_mode == 2) {
        float atmo_t = engine_get_param_f(PARAM_ATMO_THRESHOLD);
        float atmo_k = engine_get_param_f(PARAM_ATMO_KNEE);
        float dial_t = engine_get_param_f(PARAM_DIALOG_THRESHOLD);
        float dial_k = engine_get_param_f(PARAM_DIALOG_KNEE);
        float atmoR = atmo_t + atmo_k * 0.5f;
        float dialL = atmoR;
        float dialR = dial_t + dial_k * 0.5f;
        if (atmoR > x_lo) {
            float ax0 = x2px(x_lo);
            float ax1 = x2px(std::min(atmoR, x_hi));
            dl->AddRectFilled(ImVec2(ax0, plot_min.y),
                              ImVec2(ax1, plot_max.y),
                              col32(clr::atmo_purple, 0.10f));
        }
        if (dialR > dialL) {
            float dx0 = x2px(std::max(dialL, x_lo));
            float dx1 = x2px(std::min(dialR, x_hi));
            dl->AddRectFilled(ImVec2(dx0, plot_min.y),
                              ImVec2(dx1, plot_max.y),
                              col32(clr::dialog_blue, 0.10f));
        }
    }

    // Grid lines every 10 dB on both axes
    for (int v = -80; v <= 0; v += 10) {
        float x = x2px((float)v);
        dl->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y),
                    col32(clr::line_lite, 0.5f), 1.0f);
    }
    for (int v = -20; v <= 30; v += 10) {
        float y = y2px((float)v);
        dl->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y),
                    col32(clr::line_lite, 0.5f), 1.0f);
    }
    // 0 dB axis brighter
    {
        float y0 = y2px(0.0f);
        dl->AddLine(ImVec2(plot_min.x, y0), ImVec2(plot_max.x, y0),
                    col32(clr::line, 0.8f), 1.2f);
    }

    // Upward curve (mint) + downward curve (red) sampled separately
    {
        const int N = 240;
        ImVec2 up_pts[N], dn_pts[N];
        for (int i = 0; i < N; i++) {
            float t = (float)i / (N - 1);
            float xd = x_lo + t * (x_hi - x_lo);
            GainPair gp = gain_target_view(xd, arch_mode);
            float yu = gp.up; if (yu < y_lo) yu = y_lo; if (yu > y_hi) yu = y_hi;
            float yd = gp.dn; if (yd < y_lo) yd = y_lo; if (yd > y_hi) yd = y_hi;
            up_pts[i] = ImVec2(x2px(xd), y2px(yu));
            dn_pts[i] = ImVec2(x2px(xd), y2px(yd));
        }
        // Downward first so the upward line draws on top where they cross.
        if (engine_get_param_i(PARAM_DOWNWARD_EN)) {
            dl->AddPolyline(dn_pts, N, col32(clr::duck_red), 0, 1.8f);
        }
        dl->AddPolyline(up_pts, N, col32(clr::mint), 0, 2.2f);
    }

    // Live SC marker — vertical line + dot on curve
    if (std::isfinite(m.sc_db) && m.sc_db > x_lo) {
        float scx = std::min(std::max(m.sc_db, x_lo), x_hi);
        float gx = x2px(scx);
        dl->AddLine(ImVec2(gx, plot_min.y), ImVec2(gx, plot_max.y),
                    col32(clr::amber, 0.55f), 1.4f);
        float gain = std::isfinite(m.gain_db) ? m.gain_db : 0.0f;
        if (gain < y_lo) gain = y_lo;
        if (gain > y_hi) gain = y_hi;
        ImVec2 p(gx, y2px(gain));
        dl->AddCircleFilled(p, 7.0f, col32(clr::amber, 0.30f));
        dl->AddCircleFilled(p, 3.6f, col32(clr::amber, 1.00f));
    }

    // Axis labels every 20 dB
    char buf[16];
    for (int v = -80; v <= 0; v += 20) {
        float x = x2px((float)v);
        std::snprintf(buf, sizeof(buf), "%d", v);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(x - sz.x * 0.5f, plot_max.y + 4),
                    col32(clr::text_dim), buf);
    }
    for (int v = -20; v <= 30; v += 10) {
        float y = y2px((float)v);
        std::snprintf(buf, sizeof(buf), "%+d", v);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(plot_min.x - sz.x - 6, y - sz.y * 0.5f),
                    col32(clr::text_dim), buf);
    }
    // Titles
    dl->AddText(ImVec2((plot_min.x + plot_max.x) * 0.5f - 28, plot_max.y + 4),
                col32(clr::text_sand), "SC dB");
    dl->AddText(ImVec2(area_min.x + 6, plot_min.y - 14),
                col32(clr::text_sand), "Gain dB");
    // Legend on top right
    {
        ImVec2 lp(plot_max.x - 130, plot_min.y - 14);
        dl->AddText(lp, col32(clr::mint), "+ upward");
        if (engine_get_param_i(PARAM_DOWNWARD_EN))
            dl->AddText(ImVec2(lp.x + 80, lp.y), col32(clr::duck_red), "duck");
    }
}

// -----------------------------------------------------------------------------
// Vertical GR bar — used inside the meter block. Shows current gain
// reduction/lift as a centered bar (negative = duck below midline, positive
// = upward above midline).

static void draw_gr_vbar(ImVec2 area_min, ImVec2 area_max,
                         const engine_meters_t &m) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(area_min, area_max, IM_COL32(8, 8, 10, 255));
    dl->AddRect(area_min, area_max, col32(clr::line), 0.0f, 0, 1.0f);

    const float gain_lo = -20.0f, gain_hi = 30.0f;
    float y_zero = area_max.y - (0.0f - gain_lo) / (gain_hi - gain_lo)
                                * (area_max.y - area_min.y);

    // Zero baseline
    dl->AddLine(ImVec2(area_min.x, y_zero), ImVec2(area_max.x, y_zero),
                col32(clr::text_dim, 0.7f), 1.0f);

    // Tick marks every 5 dB
    for (int v = -20; v <= 30; v += 5) {
        float t = (v - gain_lo) / (gain_hi - gain_lo);
        float y = area_max.y - t * (area_max.y - area_min.y);
        dl->AddLine(ImVec2(area_min.x, y), ImVec2(area_min.x + 4, y),
                    col32(clr::text_dim, 0.5f), 1.0f);
    }

    if (std::isfinite(m.gain_db)) {
        float g = m.gain_db;
        if (g > gain_hi) g = gain_hi;
        if (g < gain_lo) g = gain_lo;
        float t = (g - gain_lo) / (gain_hi - gain_lo);
        float y = area_max.y - t * (area_max.y - area_min.y);
        if (g >= 0.0f) {
            // Upward: bar grows upward from y_zero to y
            dl->AddRectFilled(ImVec2(area_min.x + 2, y),
                              ImVec2(area_max.x - 2, y_zero),
                              col32(clr::mint, 0.85f));
        } else {
            dl->AddRectFilled(ImVec2(area_min.x + 2, y_zero),
                              ImVec2(area_max.x - 2, y),
                              col32(clr::duck_red, 0.85f));
        }
    }
}

// -----------------------------------------------------------------------------
// Peak meter strip pair (In + Out, narrow, vertical), -60..0 dB.

static void draw_peak_pair(ImVec2 origin, float strip_w, float h,
                           float in_db,  float out_db,
                           float hold_in_db, float hold_out_db,
                           const char *label, float weight) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float gap = 1.5f;

    auto db_to_pct = [](float db) {
        if (!std::isfinite(db) || db <= -60.0f) return 0.0f;
        if (db >= 0.0f) return 1.0f;
        return (db + 60.0f) / 60.0f;
    };

    auto draw_one = [&](float x0, float pct, float hold_pct, ImU32 col, ImU32 hold_col) {
        float x1 = x0 + strip_w;
        float y0 = origin.y;
        float y1 = origin.y + h;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                          IM_COL32(8, 8, 10, 255));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                    col32(clr::line_lite), 0.0f, 0, 1.0f);
        float fill_top = y1 - (y1 - y0) * pct;
        if (fill_top < y1 - 1) {
            dl->AddRectFilled(ImVec2(x0 + 1, fill_top),
                              ImVec2(x1 - 1, y1 - 1), col);
        }
        if (hold_pct > 0.01f) {
            float hy = y1 - (y1 - y0) * hold_pct;
            dl->AddLine(ImVec2(x0, hy), ImVec2(x1, hy), hold_col, 2.0f);
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
    dl->AddText(ImVec2(center - lbl_sz.x * 0.5f, origin.y + h + 3),
                col32(weight > 0.05f ? clr::text : clr::text_dim), label);
}

// Processor time this process has used since the last call - drawing and audio
// thread together, which is what shows up in htop.
static double cpu_seconds_delta() {
    static double last = 0.0;
    double t = (double)std::clock() / CLOCKS_PER_SEC;
    double d = t - last;
    last = t;
    return d;
}

// -----------------------------------------------------------------------------
// Knob descriptor + helper.

struct KnobDesc {
    const char     *label;
    engine_param_t  id;
    float           v_min, v_max;
    const char     *fmt;
};

// Slider with a sand-colored title above. Returns whether value changed.
static bool labeled_slider(const KnobDesc &k, int pushid, float width = -1.0f) {
    float v = engine_get_param_f(k.id);
    ImGui::TextColored(clr::text_sand, "%s", k.label);
    ImGui::PushID(pushid);
    if (width > 0.0f) ImGui::SetNextItemWidth(width);
    else              ImGui::SetNextItemWidth(-1);
    bool ch = ImGui::SliderFloat("##s", &v, k.v_min, k.v_max, k.fmt,
                                 ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    if (ch) engine_set_param_f(k.id, v);
    return ch;
}

// Header button — bordered, accent color when active.
static bool seg_button(const char *label, bool active, ImVec2 size,
                       ImVec4 active_color = clr::amber) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? ImVec4(active_color.x * 0.7f,
                                          active_color.y * 0.7f,
                                          active_color.z * 0.7f, 1.0f)
                                 : ImVec4(0.13f, 0.13f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          active ? active_color
                                 : ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          active ? active_color
                                 : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          active ? ImVec4(0.05f, 0.05f, 0.06f, 1.0f) : clr::text);
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
    // Seed the built-in stock presets first; state_load() then overrides
    // any the user has saved under the same name (their copy wins).
    engine_named_seed_factory();
    state_load(opts.state_path);

    // Architecture is fixed to zonal v2 (Classic/v1 retired) and the Dual
    // detector was removed. Clamp any stale state.ini back to the supported
    // set so an old file can't resurrect a dead mode.
    engine_set_param_i(PARAM_ARCHITECTURE_MODE, 2);
    if (engine_get_param_i(PARAM_DETECTOR_MODE) == 2)
        engine_set_param_i(PARAM_DETECTOR_MODE, 1);

    glfwSetErrorCallback([](int e, const char *m){ std::fprintf(stderr, "glfw: %d %s\n", e, m); });
    if (!glfwInit()) { engine_stop(); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow *win = glfwCreateWindow(1280, 760, "cinecomp", nullptr, nullptr);
    if (!win) { glfwTerminate(); engine_stop(); return 1; }
    glfwMakeContextCurrent(win);
    /* No vsync: the loop below paces itself. Leaving the swap to block for a
     * whole frame on top of that only adds latency, and on some drivers it
     * does not block at all, it spins. */
    glfwSwapInterval(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /* The surface gets out of everyone's way. The audio callback runs
     * SCHED_FIFO and is untouched by this, but the drawing shares a machine
     * with an X server, a VNC server and whatever else is in the JACK graph,
     * all of them SCHED_OTHER at nice 0. */
    if (setpriority(PRIO_PROCESS, 0, 12) != 0)
        std::fprintf(stderr, "cinecomp: nice liess sich nicht setzen\n");

    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    auto last_save = std::chrono::steady_clock::now();
    bool dirty = false;
    auto mark_dirty = [&]() { dirty = true; };

    // GUI-side meter display state — slow-decay bars + sticky holds.
    static float disp_in[ENGINE_N_CHANNELS]  = {-60,-60,-60,-60,-60,-60,-60,-60};
    static float disp_out[ENGINE_N_CHANNELS] = {-60,-60,-60,-60,-60,-60,-60,-60};
    static float hold_in[ENGINE_N_CHANNELS]  = {-60,-60,-60,-60,-60,-60,-60,-60};
    static float hold_out[ENGINE_N_CHANNELS] = {-60,-60,-60,-60,-60,-60,-60,-60};
    static float hold_age_in[ENGINE_N_CHANNELS]  = {0};
    static float hold_age_out[ENGINE_N_CHANNELS] = {0};

    /* How often the window is allowed to redraw.
     *
     * A single glfwWaitEventsTimeout() is NOT a rate limit: it returns on the
     * first event, and under a window manager that talks to its clients every
     * frame that means no limit at all - the loop then runs at whatever the
     * display will take. So the slot is sat out in a loop until it is over. */
    const double IDLE_DT = 1.0 / 10.0;
    const double BUSY_DT = 1.0 / 20.0;      /* while a control is being held */

    double t_next = 0.0, t_frame = 0.0, fps = 0.0, cpu_pct = 0.0;
    int    frames = 0;
    bool   busy = false;

    while (!glfwWindowShouldClose(win) && !g_quit) {
        for (;;) {
            double left = t_next - glfwGetTime();
            if (left <= 0.0) break;
            glfwWaitEventsTimeout(left);
        }
        glfwPollEvents();

        /* Minimised: nobody is looking, so nothing gets drawn. */
        if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
            t_next = glfwGetTime() + 0.25;
            continue;
        }

        double t_now = glfwGetTime();
        t_next = t_now + (busy ? BUSY_DT : IDLE_DT);
        frames++;
        if (t_now - t_frame >= 1.0) {
            fps     = frames / (t_now - t_frame);
            cpu_pct = 100.0 * cpu_seconds_delta() / (t_now - t_frame);
            frames  = 0;
            t_frame = t_now;
        }

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

        engine_meters_t mtr;
        engine_read_meters(&mtr);

        // Update GUI-side meter state
        {
            const float bar_dbps    = 30.0f;
            const float hold_sticky = 1.5f;
            const float hold_dbps   = 12.0f;
            float dt = io.DeltaTime;
            if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
            if (dt > 0.1f)    dt = 0.1f;
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
                upd(mtr.peak_in[ch],  disp_in[ch],  hold_in[ch],  hold_age_in[ch]);
                upd(mtr.peak_out[ch], disp_out[ch], hold_out[ch], hold_age_out[ch]);
            }
        }

        // OSC may have mutated active preset, any preset content, or the
        // named-preset library. Poll each frame and mark dirty if changed.
        {
            static int last_active = -2;
            static engine_preset_t last_presets[PRESET__COUNT] = {};
            static int  last_named_count = -1;
            static std::string last_named_active = "\x01";  // impossible sentinel
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
            int nc = engine_named_count();
            std::string na = engine_named_active();
            if (nc != last_named_count || na != last_named_active) changed = true;
            if (changed) {
                last_active = cur_active;
                for (int i = 0; i < PRESET__COUNT; i++) {
                    engine_preset_get((engine_preset_slot_t)i, &last_presets[i]);
                }
                last_named_count  = nc;
                last_named_active = na;
                snapshot_init = true;
                mark_dirty();
            }
        }

        const int arch_mode = 2;   // fixed: Classic/v1 retired, v2 only
        int det_mode  = engine_get_param_i(PARAM_DETECTOR_MODE);

        // ===== Header =================================================
        {
            ImGui::TextColored(clr::amber, "cinecomp");
            /* What the whole client costs, drawing and DSP together. */
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::TextColored(cpu_pct > 25.0 ? clr::duck_red : clr::text_dim,
                               "%.0f Bilder/s  %.0f %% CPU", fps, cpu_pct);
            ImGui::SameLine();
            ImGui::TextDisabled("· upward compressor (zonal v2)");
            ImGui::SameLine();

            // Center: presets row.  Right: arch/det/bypass.
            float center_off = ImGui::GetWindowWidth() * 0.40f;
            if (center_off < ImGui::GetCursorPosX() + 12)
                center_off = ImGui::GetCursorPosX() + 12;
            ImGui::SameLine(center_off);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            // Preset dropdown — the built-in stock presets (90ies /
            // Modern x Low/Mid/Hi) plus any user-saved named presets.
            // The generic factory LOW/MID/HIGH slots stay in the engine
            // and OSC API but are intentionally NOT listed here (kept the
            // list from being a 9-entry jumble). Copy out of the engine's
            // shared static buffer right away.
            std::string named_active = engine_named_active();
            bool has_named_active = !named_active.empty();

            char preview[ENGINE_NAME_LEN + 16];
            std::snprintf(preview, sizeof(preview), "%s%s",
                          has_named_active ? named_active.c_str() : "—",
                          (has_named_active &&
                           !live_matches_named(named_active.c_str())) ? " *" : "");

            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##fc-preset", preview)) {
                int nn = engine_named_count();
                if (nn == 0) {
                    ImGui::TextDisabled("  — keine —");
                } else {
                    for (int k = 0; k < nn; k++) {
                        const char *nm = engine_named_name(k);
                        std::string name = nm ? nm : "";
                        if (name.empty()) continue;
                        bool sel = (has_named_active && name == named_active);  // std::string ==
                        ImGui::PushID(k);
                        if (ImGui::Selectable(name.c_str(), sel)) {
                            engine_named_apply(name.c_str());
                            mark_dirty();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndCombo();
            }

            // Name input + Save/Del. Save writes current live params under
            // the typed name (create/overwrite); empty name overwrites the
            // active named preset. Del removes the active named preset.
            static char name_buf[ENGINE_NAME_LEN] = "";
            ImGui::SameLine(0, 8);
            ImGui::SetNextItemWidth(130);
            ImGui::InputTextWithHint("##fc-name", "Name…", name_buf,
                                     sizeof(name_buf));
            ImGui::SameLine(0, 6);
            if (ImGui::Button("Save", ImVec2(54, 26))) {
                std::string nm = name_buf;
                while (!nm.empty() && std::isspace((unsigned char)nm.back()))
                    nm.pop_back();
                while (!nm.empty() && std::isspace((unsigned char)nm.front()))
                    nm.erase(nm.begin());
                if (nm.empty() && has_named_active) nm = named_active;  // std::string
                if (!nm.empty()) {
                    engine_named_save(nm.c_str());
                    name_buf[0] = '\0';
                    mark_dirty();
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Save current params under the typed name "
                                  "(empty = overwrite active)");
            ImGui::SameLine();
            ImGui::BeginDisabled(!has_named_active);
            if (ImGui::Button("Del", ImVec2(44, 26))) {
                if (has_named_active) {
                    engine_named_delete(named_active.c_str());
                    engine_named_set_active("");
                    mark_dirty();
                }
            }
            if (ImGui::IsItemHovered() && has_named_active)
                ImGui::SetTooltip("Delete named preset %s", named_active.c_str());
            ImGui::EndDisabled();
            ImGui::PopStyleVar();

            // Second header line, left-aligned. Architecture is fixed to
            // zonal v2 (Classic/v1 retired), so only the detector mode and
            // bypass remain here.
            ImGui::Spacing();
            ImGui::TextColored(clr::text_sand, "Det"); ImGui::SameLine();
            const char *det_names[2] = { "RMS", "Peak" };
            for (int i = 0; i < 2; i++) {
                if (i > 0) ImGui::SameLine();
                if (seg_button(det_names[i], det_mode == i, ImVec2(46, 24),
                               clr::mint)) {
                    engine_set_param_i(PARAM_DETECTOR_MODE, i);
                    mark_dirty();
                }
            }
            ImGui::SameLine(0, 10);
            bool bp = engine_get_param_i(PARAM_BYPASS) != 0;
            ImGui::PushStyleColor(ImGuiCol_CheckMark,
                                  bp ? clr::amber : clr::mint);
            if (ImGui::Checkbox("Bypass", &bp)) {
                engine_set_param_i(PARAM_BYPASS, bp ? 1 : 0);
                mark_dirty();
            }
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // ===== Layout 2x2 =============================================
        const float footer_h = 28.0f;
        float content_h = ImGui::GetContentRegionAvail().y - footer_h - 8;
        float top_h     = content_h * 0.55f;
        float bot_h     = content_h - top_h - 6;

        float left_w  = ImGui::GetContentRegionAvail().x * 0.58f;
        float right_w = ImGui::GetContentRegionAvail().x - left_w - 8;

        // ---- Top-left: Gain-vs-SC curve --------------------------------
        ImGui::BeginChild("##curve", ImVec2(left_w, top_h), false);
        {
            ImVec2 cmin = ImGui::GetCursorScreenPos();
            ImVec2 cmax = ImVec2(cmin.x + ImGui::GetContentRegionAvail().x,
                                 cmin.y + ImGui::GetContentRegionAvail().y);
            draw_curve(cmin, cmax, mtr, arch_mode);
            ImGui::Dummy(ImVec2(cmax.x - cmin.x, cmax.y - cmin.y));
        }
        ImGui::EndChild();

        // ---- Top-right: combined meter block ---------------------------
        ImGui::SameLine();
        ImGui::BeginChild("##meters", ImVec2(right_w, top_h), true);
        {
            ImVec2 m_origin = ImGui::GetCursorScreenPos();
            float avail_w = ImGui::GetContentRegionAvail().x;
            float avail_h = ImGui::GetContentRegionAvail().y;

            // Split into: left column (GR bar + SC/Gain readouts), right (8ch peaks)
            const float left_col_w = 110.0f;
            const float gap_col    = 12.0f;
            float peak_col_w = avail_w - left_col_w - gap_col;
            if (peak_col_w < 100) peak_col_w = 100;

            // Title row. The left caption has to fit the narrow GR-bar
            // column (left_col_w) — a long string here ran straight into
            // the channel-peaks caption, so keep it short.
            ImGui::TextColored(clr::text_sand, "GR / Lift");
            ImGui::SameLine(left_col_w + gap_col);
            ImGui::TextColored(clr::text_sand, "Channel Peaks (in / out)");

            float row_top_y = ImGui::GetCursorScreenPos().y;
            float row_h = avail_h - (row_top_y - m_origin.y) - 22;
            if (row_h < 60) row_h = 60;

            // GR vertical bar
            const float gr_bar_w = 26.0f;
            ImVec2 gr_min(m_origin.x + 8, row_top_y);
            ImVec2 gr_max(gr_min.x + gr_bar_w, row_top_y + row_h);
            draw_gr_vbar(gr_min, gr_max, mtr);

            // Numeric readouts to the right of the bar
            ImDrawList *dl = ImGui::GetWindowDrawList();
            char buf[32];
            float text_x = gr_max.x + 8;
            float y = gr_min.y + 4;
            dl->AddText(ImVec2(text_x, y), col32(clr::text_dim), "SC");
            y += 14;
            if (std::isfinite(mtr.sc_db) && mtr.sc_db > -60.0f)
                std::snprintf(buf, sizeof(buf), "%.1f", mtr.sc_db);
            else
                std::strcpy(buf, "—");
            dl->AddText(ImVec2(text_x, y), col32(clr::mint), buf);
            y += 18;
            dl->AddText(ImVec2(text_x, y), col32(clr::text_dim), "Gain");
            y += 14;
            if (std::isfinite(mtr.gain_db)) {
                std::snprintf(buf, sizeof(buf), "%+.1f", mtr.gain_db);
                dl->AddText(ImVec2(text_x, y),
                            col32(mtr.gain_db >= 0 ? clr::mint : clr::duck_red), buf);
            } else {
                dl->AddText(ImVec2(text_x, y), col32(clr::text_dim), "—");
            }

            // 8-channel peak meters
            float peak_origin_x = m_origin.x + left_col_w + gap_col;
            float outer_gap = 6.0f;
            float total_gap = outer_gap * (ENGINE_N_CHANNELS - 1);
            float pair_w = (peak_col_w - total_gap) / ENGINE_N_CHANNELS;
            if (pair_w < 12) pair_w = 12;
            float strip_w = (pair_w - 1.5f) * 0.5f;
            if (strip_w < 5) strip_w = 5;
            float strip_h = row_h - 16; // leave room for label
            if (strip_h < 30) strip_h = 30;

            float x = peak_origin_x;
            for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
                draw_peak_pair(ImVec2(x, row_top_y),
                               strip_w, strip_h,
                               disp_in[ch],  disp_out[ch],
                               hold_in[ch],  hold_out[ch],
                               CHAN_NAMES[ch], mtr.weight[ch]);
                x += pair_w + outer_gap;
            }
            ImGui::Dummy(ImVec2(avail_w, row_h + 22));
        }
        ImGui::EndChild();

        // ---- Bottom-left: Upward heading + 3x3 knob matrix -------------
        ImGui::BeginChild("##upward", ImVec2(left_w, bot_h), true);
        {
            ImGui::TextColored(clr::text_sand, "UPWARD");
            ImGui::SameLine();
            ImGui::TextDisabled("(band plateaus — atmo / dialog)");
            ImGui::Separator();

            // 3 columns: Atmo | Dialog | Envelope. Heights match.
            ImGui::Columns(3, "##upcols", false);

            static const KnobDesc ATMO_KNOBS[] = {
                {"Atmo Lift",  PARAM_ATMO_MAX_GAIN,   0.0f, 30.0f, "%.1f dB"},
                {"Atmo Thr",   PARAM_ATMO_THRESHOLD, -80.0f, 0.0f, "%.1f dB"},
                {"Atmo Knee",  PARAM_ATMO_KNEE,       0.0f, 30.0f, "%.1f dB"},
            };
            for (int i = 0; i < 3; i++) {
                if (labeled_slider(ATMO_KNOBS[i], 100 + i)) mark_dirty();
                ImGui::Spacing();
            }
            ImGui::NextColumn();
            static const KnobDesc DIAL_KNOBS[] = {
                {"Dialog Lift", PARAM_DIALOG_MAX_GAIN,   0.0f, 30.0f, "%.1f dB"},
                {"Dialog Thr",  PARAM_DIALOG_THRESHOLD, -60.0f, 0.0f, "%.1f dB"},
                {"Dialog Knee", PARAM_DIALOG_KNEE,       0.0f, 30.0f, "%.1f dB"},
            };
            for (int i = 0; i < 3; i++) {
                if (labeled_slider(DIAL_KNOBS[i], 110 + i)) mark_dirty();
                ImGui::Spacing();
            }
            ImGui::NextColumn();
            static const KnobDesc ENV_KNOBS[] = {
                {"Attack",     PARAM_UPWARD_ATTACK_MS,   1.0f, 5000.0f, "%.0f ms"},
                {"Release",    PARAM_UPWARD_RELEASE_MS,  1.0f, 10000.0f, "%.0f ms"},
            };
            for (int i = 0; i < 2; i++) {
                if (labeled_slider(ENV_KNOBS[i], 120 + i)) mark_dirty();
                ImGui::Spacing();
            }
            // Empty third cell — keep alignment clean
            ImGui::Columns(1);
            // Noise floor + knee are fixed internal guards (floor -80 dB,
            // below the activity floor) — deliberately not exposed.
        }
        ImGui::EndChild();

        // ---- Bottom-right: Downward + Global ---------------------------
        ImGui::SameLine();
        ImGui::BeginChild("##downward", ImVec2(right_w, bot_h), true);
        {
            ImGui::TextColored(clr::text_sand, "DOWNWARD");
            ImGui::SameLine();
            bool d_on = engine_get_param_i(PARAM_DOWNWARD_EN) != 0;
            ImGui::PushStyleColor(ImGuiCol_CheckMark,
                                  d_on ? clr::amber : clr::mint);
            if (ImGui::Checkbox("On", &d_on)) {
                engine_set_param_i(PARAM_DOWNWARD_EN, d_on ? 1 : 0);
                mark_dirty();
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Downward limiter: tames transients above threshold.\n"
                                  "Soft knee + lookahead = transparent on real material.");
            ImGui::Separator();

            ImGui::BeginDisabled(!d_on);
            ImGui::Columns(3, "##dnrow1", false);
            static const KnobDesc DN_ROW1[] = {
                {"Thr",    PARAM_DOWN_THRESHOLD, -60.0f, 0.0f,  "%.1f dB"},
                {"Ratio",  PARAM_DOWN_RATIO,       1.0f, 20.0f, "%.1f:1"},
                {"Knee",   PARAM_KNEE_DB,          0.0f, 30.0f, "%.1f dB"},
            };
            for (int i = 0; i < 3; i++) {
                if (labeled_slider(DN_ROW1[i], 200 + i)) mark_dirty();
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
            ImGui::Spacing();

            // Row 2: duck attack / release
            ImGui::Columns(2, "##dnrow2", false);
            static const KnobDesc DN_ROW2[] = {
                {"Attack",  PARAM_DUCK_ATTACK_MS,  0.1f, 100.0f,  "%.1f ms"},
                {"Release", PARAM_DUCK_RELEASE_MS, 1.0f, 2000.0f, "%.0f ms"},
            };
            for (int i = 0; i < 2; i++) {
                if (labeled_slider(DN_ROW2[i], 210 + i)) mark_dirty();
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
            ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(clr::text_sand, "GLOBAL");
            ImGui::Columns(3, "##globalrow", false);
            // Makeup + side-chain decay + wet/dry. SC-HPF, lookahead and
            // the noise-floor guard are fixed internals — not exposed.
            static const KnobDesc GLB_KNOBS[] = {
                {"Makeup",   PARAM_MAKEUP_DB,   -12.0f, 18.0f,   "%.1f dB"},
                {"SC Decay", PARAM_RMS_WIN_MS,   10.0f, 2000.0f, "%.0f ms"},
                {"Wet/Dry",  PARAM_WET_DRY,       0.0f, 1.0f,    "%.2f"},
            };
            for (int i = 0; i < 3; i++) {
                if (labeled_slider(GLB_KNOBS[i], 220 + i)) mark_dirty();
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
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

        /* Only while a control is actually being held does the surface need to
         * move quickly; the rest of the time nobody is looking that hard. */
        busy = ImGui::IsAnyItemActive() || ImGui::GetIO().MouseDown[0];

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
