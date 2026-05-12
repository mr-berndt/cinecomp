/*
 * aroio_filmcomp standalone — Dear ImGui GUI front-end.
 *
 * Runs an OpenGL window via GLFW and an ImGui-rendered control surface
 * for the C audio engine (audio_engine.c). The engine runs JACK/OSC on
 * its own threads; the GUI just polls/pushes atomics.
 *
 * Layout mirrors the aroio6 web UI's Dynamics tab: header strip with
 * Bypass + Detector toggle, GR meter column, 8 channel peak bars,
 * 12-knob parameter grid.
 *
 * State persistence: simple INI-ish text file in
 *   $XDG_CONFIG_HOME/aroio_filmcomp/state.ini   (fallback ~/.config/…)
 * Loaded on start, saved on shutdown + every ~1 s if dirty.
 */

#include <atomic>
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
#include <unistd.h>
#include <pwd.h>

#include "audio_engine.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// -----------------------------------------------------------------------------
// State file (very small INI parser — key=value, one per line, # comments)

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
    mkdir(base.c_str(), 0755);                  // best-effort
    return base + "/state.ini";
}

static void state_save(const std::string &path) {
    std::ofstream f(path);
    if (!f) return;
    f << "# aroio_filmcomp state — auto-generated, edits survive\n";
    auto put_f = [&](const char *k, engine_param_t id) {
        f << k << " = " << engine_get_param_f(id) << "\n";
    };
    auto put_i = [&](const char *k, engine_param_t id) {
        f << k << " = " << engine_get_param_i(id) << "\n";
    };
    put_f("threshold",      PARAM_THRESHOLD);
    put_f("ratio",          PARAM_RATIO);
    put_f("attack_ms",      PARAM_ATTACK_MS);
    put_f("release_ms",     PARAM_RELEASE_MS);
    put_f("hold_ms",        PARAM_HOLD_MS);
    put_f("knee_db",        PARAM_KNEE_DB);
    put_f("makeup_db",      PARAM_MAKEUP_DB);
    put_f("wet_dry",        PARAM_WET_DRY);
    put_f("rms_win_ms",     PARAM_RMS_WIN_MS);
    put_f("sc_hpf_hz",      PARAM_SC_HPF_HZ);
    put_f("lookahead_ms",   PARAM_LOOKAHEAD_MS);
    put_f("max_gain_db",    PARAM_MAX_GAIN_DB);
    put_f("down_threshold", PARAM_DOWN_THRESHOLD);
    put_f("down_ratio",     PARAM_DOWN_RATIO);
    put_i("detector_mode",  PARAM_DETECTOR_MODE);
    put_i("downward_en",    PARAM_DOWNWARD_EN);
    put_i("bypass",         PARAM_BYPASS);
}

static void state_load(const std::string &path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        };
        trim(key); trim(val);
        if (key.empty() || val.empty()) continue;

        // Map key → param id. Float ones first.
        struct FMap { const char *k; engine_param_t id; };
        static const FMap fm[] = {
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
        bool handled = false;
        for (auto &m : fm) {
            if (key == m.k) {
                engine_set_param_f(m.id, std::strtof(val.c_str(), nullptr));
                handled = true;
                break;
            }
        }
        if (handled) continue;
        if (key == "detector_mode") engine_set_param_i(PARAM_DETECTOR_MODE, std::atoi(val.c_str()));
        else if (key == "downward_en") engine_set_param_i(PARAM_DOWNWARD_EN, std::atoi(val.c_str()));
        else if (key == "bypass")    engine_set_param_i(PARAM_BYPASS,    std::atoi(val.c_str()));
    }
}

// -----------------------------------------------------------------------------
// Theme — dark aroio-ish (mint accent, amber warning).

static void apply_theme() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding  = 2.0f;
    s.GrabRounding   = 2.0f;
    s.WindowPadding  = ImVec2(10, 10);
    s.FramePadding   = ImVec2(8, 4);
    s.ItemSpacing    = ImVec2(8, 6);

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.15f, 0.15f, 0.16f, 1.0f);
    c[ImGuiCol_Text]            = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    c[ImGuiCol_Border]          = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.42f, 0.79f, 0.66f, 1.0f);   // mint
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.95f, 0.80f, 1.0f);
    c[ImGuiCol_Button]          = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.22f, 0.22f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.30f, 0.55f, 0.45f, 1.0f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.42f, 0.79f, 0.66f, 1.0f);
    c[ImGuiCol_Header]          = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.25f, 0.25f, 0.27f, 1.0f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.30f, 0.55f, 0.45f, 1.0f);
    c[ImGuiCol_PlotHistogram]   = ImVec4(0.42f, 0.79f, 0.66f, 1.0f);
    c[ImGuiCol_PlotLines]       = ImVec4(0.42f, 0.79f, 0.66f, 1.0f);
}

// Knob descriptor for the param grid
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
    { "SC HPF",     PARAM_SC_HPF_HZ,     20.0f, 500.0f,  "%.0f Hz" },
    { "RMS Win",    PARAM_RMS_WIN_MS,    10.0f, 2000.0f, "%.0f ms" },
    { "Max Gain",   PARAM_MAX_GAIN_DB,    0.0f, 30.0f,   "%.1f dB" },
    { "Makeup",     PARAM_MAKEUP_DB,    -12.0f, 18.0f,   "%.1f dB" },
    { "Wet/Dry",    PARAM_WET_DRY,        0.0f,  1.0f,   "%.2f"    },
    { "Attack",     PARAM_ATTACK_MS,      1.0f, 200.0f,  "%.0f ms" },
    { "Release",    PARAM_RELEASE_MS,    10.0f, 2000.0f, "%.0f ms" },
    { "Hold",       PARAM_HOLD_MS,        0.0f, 200.0f,  "%.0f ms" },
    { "L-Ahead",    PARAM_LOOKAHEAD_MS,   0.0f, 20.0f,   "%.1f ms" },
};
static const int N_KNOBS = sizeof(KNOBS) / sizeof(KNOBS[0]);

static const char *CHAN_NAMES[ENGINE_N_CHANNELS] = {
    "L", "R", "C", "LFE", "LS", "RS", "RBL", "RBR"
};

// Map dBFS (-60..0) → bar fill 0..1
static float db_to_bar(float db) {
    if (!std::isfinite(db) || db <= -60.0f) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return (db + 60.0f) / 60.0f;
}

// -----------------------------------------------------------------------------
// Main render loop

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int) { g_quit = 1; }

int main(int argc, char **argv) {
    AppOpts opts;
    opts.state_path = default_state_path();

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--name") && i + 1 < argc) opts.jack_name = argv[++i];
        else if (!std::strcmp(argv[i], "--osc") && i + 1 < argc) opts.osc_port = std::atoi(argv[++i]);
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

    // ---- GLFW ----
    glfwSetErrorCallback([](int e, const char *m){ std::fprintf(stderr, "glfw: %d %s\n", e, m); });
    if (!glfwInit()) { engine_stop(); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow *win = glfwCreateWindow(900, 480, "aroio_filmcomp", nullptr, nullptr);
    if (!win) { glfwTerminate(); engine_stop(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Save-on-dirty machinery
    auto last_save = std::chrono::steady_clock::now();
    bool dirty = false;

    while (!glfwWindowShouldClose(win) && !g_quit) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Fill the whole window with one panel
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- Header: title + Detector + Bypass ----
        {
            ImGui::TextColored(ImVec4(0.42f, 0.79f, 0.66f, 1.0f), "aroio_filmcomp");
            ImGui::SameLine();
            ImGui::TextDisabled("· upward compressor for film");
            ImGui::SameLine(ImGui::GetWindowWidth() - 260);

            int mode = engine_get_param_i(PARAM_DETECTOR_MODE);
            const char *modes[] = { "RMS", "Peak" };
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("##det", &mode, modes, 2)) {
                engine_set_param_i(PARAM_DETECTOR_MODE, mode);
                dirty = true;
            }
            ImGui::SameLine();
            int bypass = engine_get_param_i(PARAM_BYPASS);
            bool bp = bypass != 0;
            if (ImGui::Checkbox("Bypass", &bp)) {
                engine_set_param_i(PARAM_BYPASS, bp ? 1 : 0);
                dirty = true;
            }
            ImGui::Separator();
        }

        engine_meters_t m;
        engine_read_meters(&m);

        ImGui::BeginChild("##meters", ImVec2(280, 200), true);
        {
            // Numeric SC / Gain
            ImGui::Columns(2, nullptr, false);
            ImGui::TextDisabled("SC dB");
            ImGui::TextColored(ImVec4(0.42f, 0.79f, 0.66f, 1.0f),
                std::isfinite(m.sc_db) ? "%.1f" : "—", m.sc_db);
            ImGui::NextColumn();
            ImGui::TextDisabled("Gain dB");
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f),
                std::isfinite(m.gain_db) ? "+%.1f" : "—", m.gain_db);
            ImGui::Columns(1);

            ImGui::Separator();

            // GR bar — vertical, grows up
            float max_g = engine_get_param_f(PARAM_MAX_GAIN_DB);
            if (max_g < 0.1f) max_g = 0.1f;
            float gr = m.gain_db < 0.0f ? 0.0f : m.gain_db;
            float gr_pct = gr / max_g; if (gr_pct > 1.0f) gr_pct = 1.0f;

            ImVec2 bar_pos = ImGui::GetCursorScreenPos();
            float bar_w = 28, bar_h = 110;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                              IM_COL32(15, 15, 17, 255));
            dl->AddRect(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                        IM_COL32(50, 50, 55, 255));
            float fill_h = bar_h * gr_pct;
            dl->AddRectFilledMultiColor(
                ImVec2(bar_pos.x + 1, bar_pos.y + bar_h - fill_h),
                ImVec2(bar_pos.x + bar_w - 1, bar_pos.y + bar_h - 1),
                IM_COL32(242, 165, 76, 255), IM_COL32(242, 165, 76, 255),
                IM_COL32(107, 200, 168, 255), IM_COL32(107, 200, 168, 255));
            ImGui::Dummy(ImVec2(bar_w, bar_h + 4));
            ImGui::Text("GR  +%.1f", gr);

            ImGui::SameLine();
            ImGui::BeginGroup();
            // 8 channel bars
            for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
                if (ch > 0) ImGui::SameLine();
                ImGui::BeginGroup();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float cw = 16, ch_h = 80;
                // input bar
                dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch_h),
                                  IM_COL32(15, 15, 17, 255));
                float in_pct = db_to_bar(m.peak_in[ch]);
                dl->AddRectFilled(
                    ImVec2(p.x + 1, p.y + ch_h - ch_h * in_pct),
                    ImVec2(p.x + cw - 1, p.y + ch_h - 1),
                    IM_COL32(70, 130, 110, 255));
                // out bar (right next to it, narrower)
                ImVec2 p2(p.x + cw + 2, p.y);
                dl->AddRectFilled(p2, ImVec2(p2.x + cw, p2.y + ch_h),
                                  IM_COL32(15, 15, 17, 255));
                float out_pct = db_to_bar(m.peak_out[ch]);
                dl->AddRectFilled(
                    ImVec2(p2.x + 1, p2.y + ch_h - ch_h * out_pct),
                    ImVec2(p2.x + cw - 1, p2.y + ch_h - 1),
                    IM_COL32(124, 180, 232, 255));
                ImGui::Dummy(ImVec2(cw * 2 + 2, ch_h + 2));
                // weight strip
                ImVec2 wp = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(wp, ImVec2(wp.x + cw * 2 + 2, wp.y + 3),
                                  IM_COL32(15, 15, 17, 255));
                float w_pct = m.weight[ch]; if (w_pct < 0) w_pct = 0; if (w_pct > 1) w_pct = 1;
                dl->AddRectFilled(wp,
                    ImVec2(wp.x + (cw * 2 + 2) * w_pct, wp.y + 3),
                    IM_COL32(242, 165, 76, 255));
                ImGui::Dummy(ImVec2(cw * 2 + 2, 6));
                ImGui::TextUnformatted(CHAN_NAMES[ch]);
                ImGui::EndGroup();
            }
            ImGui::EndGroup();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Knob grid
        ImGui::BeginChild("##knobs", ImVec2(0, 200), true);
        {
            const int cols = 4;
            ImGui::Columns(cols, nullptr, false);
            for (int i = 0; i < N_KNOBS; i++) {
                const KnobDesc &k = KNOBS[i];
                float v = engine_get_param_f(k.id);
                ImGui::TextDisabled("%s", k.label);
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##s", &v, k.v_min, k.v_max, k.fmt,
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    engine_set_param_f(k.id, v);
                    dirty = true;
                }
                ImGui::PopID();
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }
        ImGui::EndChild();

        // Footer: file path + status
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

        // Render
        ImGui::Render();
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    // Final save on exit
    state_save(opts.state_path);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    engine_stop();
    return 0;
}
