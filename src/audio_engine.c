/*
 * cinecomp — Upward / parallel compressor for film playback.
 *
 * Standalone-app DSP body. Mirrors the aroio6-Buildroot package
 * `aroio_filmcomp` (Aroio platform variant of the same engine) on a
 * per-line basis, with a small wrapper API at the bottom so the
 * ImGui front-end can drive parameters / read meters / apply presets.
 *
 * Topology: 8 inputs (7.1: L R C LFE LS RS RBL RBR) → side-chain
 * detector + gain computer → 8 outputs (1:1 channels, linked gain).
 *
 * Realtime-safe audio thread (no malloc, no locks). OSC server and
 * peak reporter run in separate threads; communication via _Atomic.
 *
 * Three architecture modes:
 *   0 = classic   — single upward stage + duck
 *   1 = zonal v1  — summed Atmo + Dialog upward stages
 *   2 = zonal v2  — band-shaped plateaus (default, 2026-05-15 tuning)
 *
 * OSC API (UDP, default port 14041):
 *   in (classic + global):
 *     /cinecomp/threshold f          — dBFS for SC
 *     /cinecomp/ratio f              — 1.0 .. 20.0 (upward)
 *     /cinecomp/attack_ms f          — how fast boost ramps up
 *     /cinecomp/release_ms f         — how fast boost decays
 *     /cinecomp/hold_ms f            — anti-pump hold between A and R
 *     /cinecomp/knee_db f            — soft-knee width (duck stage in zonal)
 *     /cinecomp/makeup_db f          — post-comp static gain
 *     /cinecomp/wet_dry f            — 0.0=dry .. 1.0=full comp
 *     /cinecomp/rms_win_ms f         — detector smoothing time
 *     /cinecomp/sc_hpf_hz f          — side-chain HPF corner
 *     /cinecomp/lookahead_ms f       — audio path delay (anti-overshoot)
 *     /cinecomp/max_gain f           — hard cap for upward boost (dB, classic)
 *     /cinecomp/detector i           — 0=RMS, 1=Peak, 2=Dual (zonal-only)
 *     /cinecomp/downward/enable i    — 0/1
 *     /cinecomp/downward/threshold f
 *     /cinecomp/downward/ratio f
 *     /cinecomp/bypass i             — 0/1 (default 1 at boot)
 *   in (zonal architecture):
 *     /cinecomp/architecture i
 *     /cinecomp/atmo/threshold f     /cinecomp/atmo/max_gain f    /cinecomp/atmo/knee f
 *     /cinecomp/dialog/threshold f   /cinecomp/dialog/max_gain f  /cinecomp/dialog/knee f
 *     /cinecomp/noise/floor f        /cinecomp/noise/knee f
 *     /cinecomp/upward/attack_ms f   /cinecomp/upward/release_ms f
 *     /cinecomp/duck/attack_ms f     /cinecomp/duck/release_ms f
 *   in (preset control):
 *     /cinecomp/preset/select i      — apply preset N (0=low 1=mid 2=high)
 *     /cinecomp/preset/save   i      — save current params to preset N
 *     /cinecomp/preset/reset  i      — reset preset N to factory defaults
 *     /cinecomp/named/save   s       — save live params under name S
 *     /cinecomp/named/apply  s       — apply named preset S
 *     /cinecomp/named/delete s       — delete named preset S
 *   in (meter subscription):
 *     /cinecomp/subscribe            — sender added to broadcast list
 *     /cinecomp/unsubscribe          — sender removed
 *     /cinecomp/get                  — request current state
 *   out (broadcast to subscribers, ~20 Hz):
 *     /cinecomp/peaks_in ff…f        — 8 peak dBFS per input channel
 *     /cinecomp/peaks_out ff…f       — 8 peak dBFS per output channel
 *     /cinecomp/weights ff…f         — 8 channel weights 0..1 (activity)
 *     /cinecomp/sc_db f              — current side-chain RMS dB
 *     /cinecomp/gain_db f            — current applied gain (before makeup)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef CINECOMP_NO_JACK
#include <jack/jack.h>
#endif

#include "audio_engine.h"

/* FTZ/DAZ: flush sub-normal floats. Same rationale as in aroio_volctl —
 * smoothing through denormals on x86 causes severe slowdowns. */
#if defined(__x86_64__) || defined(__i386__)
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#  define FC_RT_INIT_FPU() do { \
       _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON); \
       _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON); \
   } while (0)
#elif defined(__aarch64__)
#  define FC_RT_INIT_FPU() do { \
       uint64_t fpcr; \
       __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr)); \
       fpcr |= (1ULL << 24); \
       __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr)); \
   } while (0)
#else
#  define FC_RT_INIT_FPU() do { } while (0)
#endif

#define N_CHANNELS              8
#define MAX_LOOKAHEAD_SAMPLES   4800   /* 100 ms @ 48 k, comfortable margin */
#define MAX_SUBSCRIBERS         8
#define OSC_BUFSIZE             1024

/* Channel-activity sigmoid: smooth 0..1 weight over rms_db transition.
 * Inactive below ACTIVITY_FLOOR-WIDTH/2, fully active above +WIDTH/2.
 * Centered well below typical program-noise floor so true silence (~-90)
 * gets w≈0 but quiet ambient (~-60) gets w≈1. */
#define ACTIVITY_FLOOR_DB       -80.0f
#define ACTIVITY_SLOPE_DB         5.0f

/* ----------------------------- Shared state ----------------------------- */

static int osc_port = 14041;
#ifndef CINECOMP_NO_JACK
static const char *jack_name = "cinecomp";
#endif
static int verbose = 0;

#ifndef CINECOMP_NO_JACK
static jack_client_t *client;
static jack_port_t *in_ports[N_CHANNELS];
static jack_port_t *out_ports[N_CHANNELS];
#endif
static unsigned sample_rate = 48000;

/* Parameters — set by GUI/OSC, read by audio thread.
 *
 * Defaults reflect the cinema-verified 2026-05-15 tuning (zonal v2,
 * Peak detector). Atomic loads/stores keep cross-thread access safe
 * without locks. */
static _Atomic float p_threshold_db   = -10.0f;
static _Atomic float p_ratio          =   6.0f;
static _Atomic float p_attack_ms      =   5.0f;
static _Atomic float p_release_ms     = 800.0f;
static _Atomic float p_hold_ms        =   0.0f;
static _Atomic float p_knee_db        =  16.5f;  /* duck soft-knee in zonal */
static _Atomic float p_makeup_db      =   0.0f;
static _Atomic int   p_makeup_follow_dialog = 0;
static _Atomic float p_wet_dry        =   1.0f;
static _Atomic float p_rms_win_ms     =  50.0f;
static _Atomic float p_sc_hpf_hz      =  20.0f;
static _Atomic float p_lookahead_ms   =  20.0f;
static _Atomic float p_max_gain_db    =  18.5f;
/* Detector mode: 0=RMS, 1=Peak, 2=Dual (RMS feeds upward, Peak feeds duck).
 * Dual is meaningful only in zonal architecture mode — classic ignores it
 * and falls back to RMS. */
static _Atomic int   p_detector_mode  =   1;
/* Die eigentliche Wahrheit seit 21.9.2026: was jede Stufe misst.
 * p_detector_mode ist nur noch der Sammelschalter davor. */
static _Atomic int   p_det_up          =   1;   /* 0=RMS, 1=Peak */
static _Atomic int   p_det_down        =   1;
/* Anteil der Daempfung auf dem Center, in Prozent - siehe Kopfdatei.
 * 100 = wie bisher, alle acht Kanaele teilen sich eine Verstaerkung. */
static _Atomic int   p_duck_center_pct = 100;
static _Atomic int   p_downward_en    =   1;
static _Atomic float p_down_threshold = -14.0f;
static _Atomic float p_down_ratio     =   1.8f;
static _Atomic int   p_bypass         =   0;   /* standalone processes by default
                                                * (installed = meant to be used;
                                                * the Aroio buildroot variant
                                                * differs, boots bypassed) */

/* ---- Zonal architecture (architecture_mode == 1 / 2) ----
 *
 * v1 (summed): two parallel upward stages plus duck. Each upward stage
 *   has independent threshold + max_gain + knee, summed additively
 *   before smoothing.
 *
 * v2 (band-shaped): atmo / dialog are plateaus on the SC axis with
 *   smoothstep edges; no stage summing. Gain stays flat inside a zone,
 *   transitions only across zone boundaries. Pump-resistant. Live-
 *   verified across the four reference scenes (John-Wick, Quiet-Place
 *   Wecker + Bastel, Boot hoher See). */
static _Atomic int   p_architecture_mode = 2;   /* 0=classic, 1=v1, 2=v2 */
static _Atomic float p_atmo_threshold    = -35.0f;
static _Atomic float p_atmo_max_gain     =  24.0f;
static _Atomic float p_atmo_knee         =  20.0f;
static _Atomic float p_dialog_threshold  = -10.5f;
static _Atomic float p_dialog_max_gain   =  10.0f;
static _Atomic float p_dialog_knee       =  13.5f;
static _Atomic float p_noise_floor_db    = -80.0f; /* fixed guard, not in GUI */
static _Atomic float p_noise_knee_db     =  10.5f;
static _Atomic float p_upward_attack_ms  = 451.0f; /* slow rise — pause-pump guard */
static _Atomic float p_upward_release_ms =  20.0f; /* fast fall — no transient amp */
static _Atomic float p_duck_attack_ms    =   9.0f;
static _Atomic float p_duck_release_ms   =  51.0f;

/* Audio-thread-local state (only audio thread writes & reads).
 * Initialised once in audio_callback first run. */
static float rms_state[N_CHANNELS];
static float peak_state[N_CHANNELS];   /* peak-follower output, mode>=1 only */
/* Second peak follower fed only when detector_mode==2 (dual), used by the
 * duck stage in zonal architecture. Independent decay tied to
 * duck_release_ms so the duck sees a true peak envelope while the upward
 * stages see RMS. */
static float peak_state_duck[N_CHANNELS];
static float sc_hpf_z1[N_CHANNELS], sc_hpf_z2[N_CHANNELS];
static float hpf_b0, hpf_b1, hpf_b2, hpf_a1, hpf_a2;
static float current_hpf_hz = 0.0f;
static float gain_current_db = 0.0f;
/* Zonal mode runs split envelope followers — upward and duck have their
 * own gain state and asymmetric time constants. Classic mode leaves
 * these untouched and uses gain_current_db. */
static float gain_upward_current_db = 0.0f;
static float gain_duck_current_db   = 0.0f;
static int   hold_counter = 0;
static float delay_buf[N_CHANNELS][MAX_LOOKAHEAD_SAMPLES];
static int   delay_pos = 0;
static int   delay_initialised = 0;

/* Meters — written by audio thread (atomic), read by peak reporter. */
/* One set per consumer. The local GUI reads twenty times a second, the
 * network reporter fifty, and both clear what they read: with a single set
 * the GUI only ever saw a fraction of each interval - most of the sound
 * never reached the meter. Each reader now clears its own. */
#define METER_LOCAL     0
#define METER_NET       1
#define METER_CONSUMERS 2
static _Atomic float peak_in_acc[METER_CONSUMERS][N_CHANNELS];
static _Atomic float peak_out_acc[METER_CONSUMERS][N_CHANNELS];
/* Loudest side-chain value since each consumer last looked. A peak hold needs
 * the loudest moment, not the one value that happened to be current. */
static _Atomic float sc_max_acc[METER_CONSUMERS] = { -120.0f, -120.0f };
static _Atomic float weight_acc[N_CHANNELS];
static _Atomic float current_sc_db    = -INFINITY;
/* Der Sidechain, den die Abwaertsstufe sieht. Im Dual-Betrieb ist das der
 * Peak und damit ein anderer Wert als current_sc_db - genau der Unterschied,
 * der die Duck-Flanke im Bild an die falsche Stelle malt. */
static _Atomic float current_sc_dn_db = -INFINITY;
static _Atomic float current_gain_atomic_db = 0.0f;

static int osc_socket = -1;
static struct sockaddr_in subscribers[MAX_SUBSCRIBERS];
/* Wann sich jeder zuletzt gemeldet hat. Ohne das blieb ein Platz fuer immer
 * belegt: wer einmal drinstand, stand bis zum Neustart drin, und nach acht
 * verschiedenen Adressen bekam niemand Neues mehr etwas - die Anzeige sagte
 * dann "no connection", obwohl die Engine munter an tote Adressen sendete.
 * Die Bruecke erneuert ihr /subscribe alle 3 s, also ist eine Frist von 10 s
 * reichlich und kostet kein Protokoll. */
#define SUBSCRIBER_TTL 10
static time_t subscriber_seen[MAX_SUBSCRIBERS];
static int subscriber_count = 0;
static pthread_mutex_t subscriber_mtx = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t running = 1;

/* ----------------------------- Utilities -------------------------------- */

static inline float db_to_lin(float db) {
	if (db <= -120.0f) return 0.0f;
	return powf(10.0f, db / 20.0f);
}

static inline float lin_to_db(float lin) {
	if (lin <= 1e-12f) return -120.0f;
	return 20.0f * log10f(lin);
}

static inline float fclampf(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* Smoothstep 0→1 over [lo, hi]. Cubic Hermite. Branchless on the
 * inner path; cheap and avoids expf() in the audio loop. */
static inline float smoothstep_f(float lo, float hi, float x) {
	if (x <= lo) return 0.0f;
	if (x >= hi) return 1.0f;
	float t = (x - lo) / (hi - lo);
	return t * t * (3.0f - 2.0f * t);
}

/* Cubic-smoothstep S(t) = 3t² - 2t³ on [0,1], clamped at the edges.
 * C¹-continuous so the zonal gain curve has no audible corners as the
 * user drags a threshold knob across the SC the audio currently occupies. */
static inline float smoothstep01(float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

/* Zone activity weight, centered at `center_db`, with `width_db` full
 * transition width. Returns 1 on the LEFT side (sc << center), 0 on the
 * RIGHT (sc >> center). Used by the v2 band-shaped zonal architecture
 * to mark out atmo / dialog / action regions on the SC axis. */
static inline float zone_active_below(float sc_db, float center_db, float width_db) {
	if (width_db < 0.001f) return sc_db <= center_db ? 1.0f : 0.0f;
	float half = width_db * 0.5f;
	float t = (center_db + half - sc_db) / width_db;
	return smoothstep01(t);
}
static inline float zone_active_above(float sc_db, float center_db, float width_db) {
	return 1.0f - zone_active_below(sc_db, center_db, width_db);
}

/* Atmo-creep guard: returns scale factor in [0,1] for the atmo-stage
 * lift, based on how far SC is below noise_floor_db. SC >= noise_floor
 * returns 1.0 (no attenuation). SC <= noise_floor - knee returns 0.0
 * (lift fully suppressed). Smoothed quadratic between for click-free
 * transition. Effect: a real room-tone floor (e.g. -75 dB camera hiss)
 * doesn't get amplified into audible noise, while a -50 dB film atmo
 * gets full lift. */
static inline float noise_floor_attenuation(float sc_db, float noise_floor_db,
                                             float noise_knee_db) {
	if (noise_knee_db < 0.001f) {
		return sc_db >= noise_floor_db ? 1.0f : 0.0f;
	}
	if (sc_db >= noise_floor_db) return 1.0f;
	if (sc_db <= noise_floor_db - noise_knee_db) return 0.0f;
	/* Smoothstep: t = (sc - lo) / knee, mapped via 3t² - 2t³. */
	float t = (sc_db - (noise_floor_db - noise_knee_db)) / noise_knee_db;
	return t * t * (3.0f - 2.0f * t);
}

static void on_signal(int sig) {
	(void)sig;
	running = 0;
}

/* ----------------------------- Biquad design ---------------------------- */

/* 2nd-order Butterworth HPF (Q = 1/sqrt(2)), RBJ cookbook form.
 * Computed lazily when sc_hpf_hz changes — rare event, not in audio
 * inner loop. */
static void update_hpf_coeffs(float fc, float fs) {
	if (fc <= 1.0f || fs <= 1.0f) return;
	float omega = 2.0f * M_PI * fc / fs;
	float cos_w = cosf(omega);
	float sin_w = sinf(omega);
	float Q = 0.70710678f;
	float alpha = sin_w / (2.0f * Q);

	float b0 =  (1.0f + cos_w) / 2.0f;
	float b1 = -(1.0f + cos_w);
	float b2 =  (1.0f + cos_w) / 2.0f;
	float a0 =   1.0f + alpha;
	float a1 =  -2.0f * cos_w;
	float a2 =   1.0f - alpha;

	/* Normalize against a0, store. */
	hpf_b0 = b0 / a0;
	hpf_b1 = b1 / a0;
	hpf_b2 = b2 / a0;
	hpf_a1 = a1 / a0;
	hpf_a2 = a2 / a0;
}

/* Direct-Form-II Transposed biquad — most numerically stable form,
 * minimal state (z1, z2). */
static inline float biquad_step(float x, float *z1, float *z2,
                                float b0, float b1, float b2,
                                float a1, float a2) {
	float y  = b0 * x + *z1;
	*z1 = b1 * x - a1 * y + *z2;
	*z2 = b2 * x - a2 * y;
	return y;
}

/* ----------------------------- Upward-gain math ------------------------- */

/* Quadratic soft-knee around threshold. Inputs in dB; returns positive
 * gain dB to ADD to signal for upward, or 0 above the knee. */
static inline float upward_gain_db(float sc_db, float threshold_db,
                                    float ratio, float knee_w) {
	float diff = threshold_db - sc_db;  /* positive when below threshold */
	float ratio_factor = 1.0f - 1.0f / ratio;

	if (knee_w < 0.001f) {
		return diff > 0.0f ? diff * ratio_factor : 0.0f;
	}

	float half_knee = knee_w * 0.5f;
	if (diff <= -half_knee) return 0.0f;
	if (diff >=  half_knee) return diff * ratio_factor;

	/* Quadratic interpolation across the knee. At the upper edge
	 * (diff = +W/2) the formula matches gain_full = (W/2) * factor. */
	float x = diff + half_knee;     /* 0..W */
	return (x * x) / (2.0f * knee_w) * ratio_factor;
}

/* Downward limiter, mirror of upward. Returns NEGATIVE gain dB
 * (reduction). */
static inline float downward_gain_db(float sc_db, float threshold_db,
                                      float ratio, float knee_w) {
	float diff = sc_db - threshold_db;
	float ratio_factor = 1.0f - 1.0f / ratio;

	if (knee_w < 0.001f) {
		return diff > 0.0f ? -diff * ratio_factor : 0.0f;
	}

	float half_knee = knee_w * 0.5f;
	if (diff <= -half_knee) return 0.0f;
	if (diff >=  half_knee) return -diff * ratio_factor;

	float x = diff + half_knee;
	return -(x * x) / (2.0f * knee_w) * ratio_factor;
}

/* ----------------------------- Audio callback --------------------------- */

/* The DSP proper. Identical to what the JACK callback always ran; it only
 * takes its buffers as arguments now, so a non-JACK host (the LADSPA plugin)
 * can drive the very same code. */
void engine_process_block(const float *const *in_bufs, float *const *out_bufs,
                          unsigned nframes) {
	static int rt_init_done = 0;
	if (!rt_init_done) {
		FC_RT_INIT_FPU();
		rt_init_done = 1;
	}

	/* One-time init of large RT state. */
	if (!delay_initialised) {
		for (int ch = 0; ch < N_CHANNELS; ch++) {
			for (int i = 0; i < MAX_LOOKAHEAD_SAMPLES; i++)
				delay_buf[ch][i] = 0.0f;
			rms_state[ch] = 1e-18f;
			peak_state[ch] = 0.0f;
			peak_state_duck[ch] = 0.0f;
			sc_hpf_z1[ch] = sc_hpf_z2[ch] = 0.0f;
		}
		delay_initialised = 1;
	}

	/* Snapshot params for this block. */
	const float threshold     = atomic_load_explicit(&p_threshold_db, memory_order_relaxed);
	const float ratio         = atomic_load_explicit(&p_ratio,        memory_order_relaxed);
	const float attack_ms     = atomic_load_explicit(&p_attack_ms,    memory_order_relaxed);
	const float release_ms    = atomic_load_explicit(&p_release_ms,   memory_order_relaxed);
	const float hold_ms       = atomic_load_explicit(&p_hold_ms,      memory_order_relaxed);
	const float knee_db       = atomic_load_explicit(&p_knee_db,      memory_order_relaxed);
	const float makeup_set    = atomic_load_explicit(&p_makeup_db,    memory_order_relaxed);
	const int   makeup_follow = atomic_load_explicit(&p_makeup_follow_dialog, memory_order_relaxed);
	const float wet_dry       = fclampf(atomic_load_explicit(&p_wet_dry, memory_order_relaxed), 0.0f, 1.0f);
	const float rms_win_ms    = atomic_load_explicit(&p_rms_win_ms,   memory_order_relaxed);
	const float sc_hpf_hz     = atomic_load_explicit(&p_sc_hpf_hz,    memory_order_relaxed);
	const float lookahead_ms  = atomic_load_explicit(&p_lookahead_ms, memory_order_relaxed);
	const float max_gain_db   = atomic_load_explicit(&p_max_gain_db,  memory_order_relaxed);
	const int   duck_center_pct = atomic_load_explicit(&p_duck_center_pct, memory_order_relaxed);
	const int   det_up        = atomic_load_explicit(&p_det_up,   memory_order_relaxed);
	const int   det_down      = atomic_load_explicit(&p_det_down, memory_order_relaxed);
	const int   downward_en   = atomic_load_explicit(&p_downward_en,  memory_order_relaxed);
	const float down_thr      = atomic_load_explicit(&p_down_threshold, memory_order_relaxed);
	const float down_ratio    = atomic_load_explicit(&p_down_ratio,   memory_order_relaxed);
	const int   bypass        = atomic_load_explicit(&p_bypass,       memory_order_relaxed);

	const int   arch_mode     = atomic_load_explicit(&p_architecture_mode, memory_order_relaxed);
	const float atmo_thr      = atomic_load_explicit(&p_atmo_threshold,    memory_order_relaxed);
	const float atmo_max      = atomic_load_explicit(&p_atmo_max_gain,     memory_order_relaxed);
	const float atmo_knee     = atomic_load_explicit(&p_atmo_knee,         memory_order_relaxed);
	const float dialog_thr    = atomic_load_explicit(&p_dialog_threshold,  memory_order_relaxed);
	const float dialog_max    = atomic_load_explicit(&p_dialog_max_gain,   memory_order_relaxed);
	const float dialog_knee   = atomic_load_explicit(&p_dialog_knee,       memory_order_relaxed);

	/* Coupled: give back exactly what the dialogue zone was allowed to add.
	 * Has to sit after dialog_max is read, hence down here rather than with
	 * the other gains. */
	const float makeup_db     = makeup_follow ? -dialog_max : makeup_set;
	const float noise_floor   = atomic_load_explicit(&p_noise_floor_db,    memory_order_relaxed);
	const float noise_knee    = atomic_load_explicit(&p_noise_knee_db,     memory_order_relaxed);
	const float up_attack_ms  = atomic_load_explicit(&p_upward_attack_ms,  memory_order_relaxed);
	const float up_release_ms = atomic_load_explicit(&p_upward_release_ms, memory_order_relaxed);
	const float dk_attack_ms  = atomic_load_explicit(&p_duck_attack_ms,    memory_order_relaxed);
	const float dk_release_ms = atomic_load_explicit(&p_duck_release_ms,   memory_order_relaxed);

	/* Update HPF coefficients lazily on freq change. */
	if (fabsf(sc_hpf_hz - current_hpf_hz) > 0.5f) {
		update_hpf_coeffs(sc_hpf_hz, (float)sample_rate);
		current_hpf_hz = sc_hpf_hz;
	}

	/* Compute smoothing alphas in per-sample form. tau_samples =
	 * (time_ms / 1000) * fs; alpha = 1 - exp(-1/tau_samples). */
	const float fs = (float)sample_rate;
	const float rms_alpha     = 1.0f - expf(-1.0f / fmaxf(1.0f, (rms_win_ms    * 0.001f) * fs));
	/* Peak-mode release: same time-constant as the RMS smoother but
	 * applied only on decay. Attack is instant (capture). */
	const float peak_decay    = expf(-1.0f / fmaxf(1.0f, (rms_win_ms * 0.001f) * fs));
	const float attack_alpha  = 1.0f - expf(-1.0f / fmaxf(1.0f, (attack_ms     * 0.001f) * fs));
	const float release_alpha = 1.0f - expf(-1.0f / fmaxf(1.0f, (release_ms    * 0.001f) * fs));
	const int   hold_samples  = (int)((hold_ms * 0.001f) * fs);
	const int   look_samples  = fclampf((lookahead_ms * 0.001f) * fs, 0, MAX_LOOKAHEAD_SAMPLES - 1);

	/* Zonal-mode alphas: separate envelopes per stage. Upward gets its
	 * own slow attack so dialog pauses don't pump atmo up audibly. Duck
	 * keeps fast attack to catch transients. */
	const float up_attack_alpha  = 1.0f - expf(-1.0f / fmaxf(1.0f, (up_attack_ms  * 0.001f) * fs));
	const float up_release_alpha = 1.0f - expf(-1.0f / fmaxf(1.0f, (up_release_ms * 0.001f) * fs));
	const float dk_attack_alpha  = 1.0f - expf(-1.0f / fmaxf(1.0f, (dk_attack_ms  * 0.001f) * fs));
	const float dk_release_alpha = 1.0f - expf(-1.0f / fmaxf(1.0f, (dk_release_ms * 0.001f) * fs));
	/* Duck-path peak decay (used only when detector_mode==2): tied to
	 * duck_release_ms so the peak envelope releases at the same rate the
	 * gain envelope is allowed to decay. */
	const float peak_decay_duck = expf(-1.0f / fmaxf(1.0f, (dk_release_ms * 0.001f) * fs));

	float *in[N_CHANNELS], *out[N_CHANNELS];
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		in[ch]  = (float *)in_bufs[ch];
		out[ch] = out_bufs[ch];
	}

	/* Per-block peak accumulators (local, then published atomic at block end). */
	float peak_in_block[N_CHANNELS]  = {0};
	float peak_out_block[N_CHANNELS] = {0};
	float weight_block[N_CHANNELS]   = {0};
	float sc_block_last = -120.0f;
	float sc_block_max  = -120.0f;
	float sc_dn_block_last = -120.0f;
	float gain_block_last = 0.0f;

	/* Der eigene Peak-Folger des Duckers, dessen Abklingen an sein Release
	 * gekoppelt ist. Er laeuft weiterhin GENAU in der alten Dual-Kombination
	 * - zonal, oben RMS, unten Peak. Bei Peak/Peak benutzt der Ducker wie
	 * bisher den gemeinsamen Folger; das ist nicht unbedingt das Richtige,
	 * aber es ist das, was der alte Modus 1 tat, und eine stille Aenderung
	 * daran wuerde bestehende Presets anders klingen lassen. */
	const int need_duck_peak = ((arch_mode == 1 || arch_mode == 2)
	                            && det_up == 0 && det_down == 1);

	for (unsigned i = 0; i < nframes; i++) {
		/* ---- Per-channel detection ----
		 *
		 * Compute RMS and Peak follower outputs for every channel,
		 * unconditionally. Zonal-dual mode needs both, classic modes
		 * use only one — but the CPU cost of always computing both is
		 * negligible (~one mul-add + one comparison per channel per
		 * sample) and the dispatch becomes simpler. */
		float sum_w = 0.0f;
		float sum_we_rms = 0.0f, sum_we_peak = 0.0f, sum_we_peak_dk = 0.0f;

		for (int ch = 0; ch < N_CHANNELS; ch++) {
			float x = in[ch][i];
			float ax = fabsf(x);
			if (ax > peak_in_block[ch]) peak_in_block[ch] = ax;

			/* SC-HPF on the per-channel signal. */
			float y = biquad_step(x, &sc_hpf_z1[ch], &sc_hpf_z2[ch],
			                       hpf_b0, hpf_b1, hpf_b2,
			                       hpf_a1, hpf_a2);
			float ay = fabsf(y);

			/* RMS smoother (always running). */
			rms_state[ch] += (y * y - rms_state[ch]) * rms_alpha;
			float lvl_rms = sqrtf(rms_state[ch] + 1e-20f);

			/* Peak follower (always running). */
			if (ay > peak_state[ch]) peak_state[ch] = ay;
			else                     peak_state[ch] *= peak_decay;
			float lvl_peak = peak_state[ch];

			/* Duck-specific peak follower (zonal+dual only): independent
			 * decay tied to duck_release_ms so the duck stage sees a
			 * true peak envelope matched to its own release time. */
			float lvl_peak_dk = lvl_peak;
			if (need_duck_peak) {
				if (ay > peak_state_duck[ch]) peak_state_duck[ch] = ay;
				else                          peak_state_duck[ch] *= peak_decay_duck;
				lvl_peak_dk = peak_state_duck[ch];
			}

			/* Activity weight: classic uses the active detector for
			 * backward compatibility, zonal always uses RMS (more
			 * stable, click-only channels don't get over-weighted). */
			float weight_level = (arch_mode == 0 && det_up == 1)
			                       ? lvl_peak : lvl_rms;
			float level_db = lin_to_db(weight_level);
			float w = smoothstep_f(
				ACTIVITY_FLOOR_DB - ACTIVITY_SLOPE_DB,
				ACTIVITY_FLOOR_DB + ACTIVITY_SLOPE_DB,
				level_db);
			if (w > weight_block[ch]) weight_block[ch] = w;

			sum_w           += w;
			sum_we_rms      += w * lvl_rms     * lvl_rms;
			sum_we_peak     += w * lvl_peak    * lvl_peak;
			sum_we_peak_dk  += w * lvl_peak_dk * lvl_peak_dk;
		}

		/* SC = energy-mean of active channels for each detector path. */
		float sc_rms_db, sc_peak_db, sc_peak_dk_db;
		if (sum_w > 1e-6f) {
			sc_rms_db     = lin_to_db(sqrtf(sum_we_rms     / sum_w));
			sc_peak_db    = lin_to_db(sqrtf(sum_we_peak    / sum_w));
			sc_peak_dk_db = lin_to_db(sqrtf(sum_we_peak_dk / sum_w));
		} else {
			sc_rms_db = sc_peak_db = sc_peak_dk_db = -120.0f;
		}

		/* Jede Stufe nimmt, was fuer sie eingestellt ist. Der Ducker
		 * greift auf seinen eigenen Peak-Folger zurueck, wenn dieser
		 * laeuft - siehe need_duck_peak. */
		float sc_up_db = (det_up == 1) ? sc_peak_db : sc_rms_db;
		float sc_dn_db = (det_down == 1)
		                   ? (need_duck_peak ? sc_peak_dk_db : sc_peak_db)
		                   : sc_rms_db;
		/* Reporter publishes the upward-side SC (primary for the user). */
		sc_block_last = sc_up_db;
		if (sc_up_db > sc_block_max) sc_block_max = sc_up_db;
		sc_dn_block_last = sc_dn_db;

		/* ---- Gain target ---- */
		float gain_target_db;

		if (arch_mode == 1 || arch_mode == 2) {
			/* ===== Zonal architecture =====
			 * v1 (mode 1, summed): Two parallel upward stages
			 *   (atmo + dialog), each capped by their own max_gain.
			 *   The sum can give atmo_max + dialog_max at very low SC.
			 *   Atmo throttled by the noise-floor knee.
			 *
			 * v2 (mode 2, band-shaped): Atmo and dialog are PLATEAUS
			 *   on the SC axis, each with smoothstep edges. No
			 *   summing of slopes — gain stays flat within a zone,
			 *   transitions only across zone boundaries. */
			float atmo_lift, dialog_lift;
			if (arch_mode == 2) {
				/* v2: smoothstep weights. atmo_thr is the
				 * atmo↔dialog boundary, atmo_knee its transition
				 * width. dialog_thr is the dialog↔action boundary,
				 * dialog_knee its width. */
				float w_atmo  = zone_active_below(sc_up_db, atmo_thr, atmo_knee);
				float w_dial  = zone_active_above(sc_up_db, atmo_thr, atmo_knee)
				              * zone_active_below(sc_up_db, dialog_thr, dialog_knee);
				float noise_w = noise_floor_attenuation(sc_up_db, noise_floor, noise_knee);
				atmo_lift   = atmo_max  * w_atmo * noise_w;
				dialog_lift = dialog_max * w_dial;
			} else {
				/* v1: summed compressor stages. */
				atmo_lift = upward_gain_db(sc_up_db, atmo_thr, ratio, atmo_knee);
				if (atmo_lift > atmo_max) atmo_lift = atmo_max;
				atmo_lift *= noise_floor_attenuation(sc_up_db, noise_floor, noise_knee);
				dialog_lift = upward_gain_db(sc_up_db, dialog_thr, ratio, dialog_knee);
				if (dialog_lift > dialog_max) dialog_lift = dialog_max;
			}

			float gain_up_target = atmo_lift + dialog_lift;
			float gain_dn_target = downward_en
			                        ? downward_gain_db(sc_dn_db, down_thr, down_ratio, knee_db)
			                        : 0.0f;

			/* Split envelope: upward has its own slow attack so
			 * dialog pauses don't pump atmo up audibly. Duck has its
			 * own fast attack so transients are caught instantly. */
			if (gain_up_target > gain_upward_current_db) {
				gain_upward_current_db += (gain_up_target - gain_upward_current_db) * up_attack_alpha;
			} else {
				gain_upward_current_db += (gain_up_target - gain_upward_current_db) * up_release_alpha;
			}
			/* Duck: gain is ≤ 0. "Engaging" = becoming more negative
			 * = gain_dn_target < current. That direction gets
			 * duck_attack_alpha. Release back to 0 gets duck_release_alpha. */
			if (gain_dn_target < gain_duck_current_db) {
				gain_duck_current_db += (gain_dn_target - gain_duck_current_db) * dk_attack_alpha;
			} else {
				gain_duck_current_db += (gain_dn_target - gain_duck_current_db) * dk_release_alpha;
			}
			gain_current_db = gain_upward_current_db + gain_duck_current_db;
			gain_target_db = gain_up_target + gain_dn_target;
		} else {
			/* ===== Classic architecture (unchanged math) ===== */
			float gain_up   = upward_gain_db(sc_up_db, threshold, ratio, knee_db);
			/* Hard cap on upward boost: when very low-level program
			 * (or leftover noise floor) drives the SC far below
			 * threshold, (T - sc) * (1 - 1/R) grows without bound
			 * and produces audible noise pumping. Clamp before
			 * adding makeup. */
			if (gain_up > max_gain_db) gain_up = max_gain_db;
			float gain_down = downward_en
			                  ? downward_gain_db(sc_dn_db, down_thr, down_ratio, knee_db)
			                  : 0.0f;
			gain_target_db = gain_up + gain_down;

			/* ---- Smoothing with attack/release + hold ----
			 * "Attack" = boost ANSTEIGT (gain_target > gain_current).
			 * "Release" = boost FÄLLT (gain_target < gain_current).
			 * Hold counter prevents premature release during short SC dips. */
			if (gain_target_db > gain_current_db) {
				/* Boost rises: attack phase. Hold has no effect here —
				 * hold gates release, not attack. */
				gain_current_db += (gain_target_db - gain_current_db) * attack_alpha;
			} else if (gain_target_db < gain_current_db) {
				/* Boost falls: release phase, gated by hold. */
				if (hold_counter > 0) {
					hold_counter--;
				} else {
					gain_current_db += (gain_target_db - gain_current_db) * release_alpha;
				}
			} else {
				/* No change. Reset hold when we hit steady state. */
				hold_counter = hold_samples;
			}
		}  /* end classic-architecture branch */

		gain_block_last = gain_current_db;

		float gain_lin = db_to_lin(gain_current_db + makeup_db);
		/* Der Center bekommt die Anhebung voll und die Daempfung nur
		 * anteilig - siehe PARAM_DUCK_CENTER_PCT. Nur in der zonalen
		 * Architektur, denn nur dort wird der Duck-Anteil getrennt
		 * gefuehrt. */
		float gain_lin_c = gain_lin;
		if (duck_center_pct < 100 && (arch_mode == 1 || arch_mode == 2))
			gain_lin_c = db_to_lin(gain_upward_current_db
			                       + gain_duck_current_db * (duck_center_pct * 0.01f)
			                       + makeup_db);

		/* ---- Apply with look-ahead delay ----
		 * Ring buffer: write current sample, read sample from
		 * `look_samples` ago. Both wrap modulo MAX_LOOKAHEAD_SAMPLES. */
		int write_pos = delay_pos;
		int read_pos  = (write_pos - look_samples + MAX_LOOKAHEAD_SAMPLES) % MAX_LOOKAHEAD_SAMPLES;

		for (int ch = 0; ch < N_CHANNELS; ch++) {
			float in_now = in[ch][i];
			delay_buf[ch][write_pos] = in_now;
			float in_delayed = delay_buf[ch][read_pos];

			float y;
			if (bypass) {
				/* Pass through with delay — preserve lipsync. */
				y = in_delayed;
			} else {
				float wet = in_delayed * ((ch == 2) ? gain_lin_c : gain_lin);
				y = wet_dry * wet + (1.0f - wet_dry) * in_delayed;
			}
			out[ch][i] = y;

			float ay = fabsf(y);
			if (ay > peak_out_block[ch]) peak_out_block[ch] = ay;
		}

		delay_pos = (delay_pos + 1) % MAX_LOOKAHEAD_SAMPLES;
	}

	/* Publish per-block meter results (max-of-stored-vs-block). */
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		for (int c = 0; c < METER_CONSUMERS; c++) {
			float prev_in = atomic_load_explicit(&peak_in_acc[c][ch], memory_order_relaxed);
			if (peak_in_block[ch] > prev_in) {
				atomic_store_explicit(&peak_in_acc[c][ch], peak_in_block[ch], memory_order_relaxed);
			}
			float prev_out = atomic_load_explicit(&peak_out_acc[c][ch], memory_order_relaxed);
			if (peak_out_block[ch] > prev_out) {
				atomic_store_explicit(&peak_out_acc[c][ch], peak_out_block[ch], memory_order_relaxed);
			}
		}
		atomic_store_explicit(&weight_acc[ch], weight_block[ch], memory_order_relaxed);
	}
	for (int c = 0; c < METER_CONSUMERS; c++) {
		float prev_sc = atomic_load_explicit(&sc_max_acc[c], memory_order_relaxed);
		if (sc_block_max > prev_sc)
			atomic_store_explicit(&sc_max_acc[c], sc_block_max, memory_order_relaxed);
	}
	atomic_store_explicit(&current_sc_db,           sc_block_last,   memory_order_relaxed);
	atomic_store_explicit(&current_sc_dn_db,        sc_dn_block_last, memory_order_relaxed);
	atomic_store_explicit(&current_gain_atomic_db,  gain_block_last, memory_order_relaxed);
}

#ifndef CINECOMP_NO_JACK
/* JACK entry point: fetch the port buffers, then run the shared DSP. */
static int audio_callback(jack_nframes_t nframes, void *arg) {
	(void)arg;
	const float *in[N_CHANNELS];
	float *out[N_CHANNELS];
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		in[ch]  = jack_port_get_buffer(in_ports[ch], nframes);
		out[ch] = jack_port_get_buffer(out_ports[ch], nframes);
	}
	engine_process_block(in, out, nframes);
	return 0;
}
#endif

void engine_set_sample_rate(unsigned sr) {
	if (sr) sample_rate = sr;
	/* Force HPF recompute next audio block. */
	current_hpf_hz = 0.0f;
}

#ifndef CINECOMP_NO_JACK
static int sample_rate_callback(jack_nframes_t nframes, void *arg) {
	(void)arg;
	engine_set_sample_rate(nframes);
	return 0;
}
#endif

/* ----------------------------- OSC ------------------------------------- */

static int osc_read_string(const uint8_t *buf, int len, int off,
                           char *out, int out_max) {
	int i = 0;
	while (off + i < len && buf[off + i] != 0 && i < out_max - 1) {
		out[i] = buf[off + i];
		i++;
	}
	out[i] = 0;
	int total = i + 1;
	int padded = (total + 3) & ~3;
	return off + padded;
}

static int osc_pack_string(uint8_t *buf, int off, const char *s) {
	int n = strlen(s);
	memcpy(buf + off, s, n);
	buf[off + n] = 0;
	int padded = (n + 1 + 3) & ~3;
	for (int i = n + 1; i < padded; i++) buf[off + i] = 0;
	return off + padded;
}

static int osc_pack_float(uint8_t *buf, int off, float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	uint32_t be = htonl(u);
	memcpy(buf + off, &be, 4);
	return off + 4;
}

static int osc_pack_int(uint8_t *buf, int off, int32_t i) {
	uint32_t be = htonl((uint32_t)i);
	memcpy(buf + off, &be, 4);
	return off + 4;
}

static float osc_read_float(const uint8_t *buf, int off) {
	uint32_t be;
	memcpy(&be, buf + off, 4);
	uint32_t u = ntohl(be);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static int32_t osc_read_int(const uint8_t *buf, int off) {
	uint32_t be;
	memcpy(&be, buf + off, 4);
	return (int32_t)ntohl(be);
}

static void send_one_float(struct sockaddr_in *dst, const char *path, float v) {
	uint8_t buf[OSC_BUFSIZE];
	int off = osc_pack_string(buf, 0, path);
	off = osc_pack_string(buf, off, ",f");
	off = osc_pack_float(buf, off, v);
	sendto(osc_socket, buf, off, 0, (struct sockaddr*)dst, sizeof(*dst));
}

static void send_one_int(struct sockaddr_in *dst, const char *path, int v) {
	uint8_t buf[OSC_BUFSIZE];
	int off = osc_pack_string(buf, 0, path);
	off = osc_pack_string(buf, off, ",i");
	off = osc_pack_int(buf, off, v);
	sendto(osc_socket, buf, off, 0, (struct sockaddr*)dst, sizeof(*dst));
}

/* Send 8 floats with /path ,ffffffff <8 floats>. */
static void send_eight_floats(struct sockaddr_in *dst, const char *path, const float *vals) {
	uint8_t buf[OSC_BUFSIZE];
	int off = osc_pack_string(buf, 0, path);
	off = osc_pack_string(buf, off, ",ffffffff");
	for (int i = 0; i < 8; i++) off = osc_pack_float(buf, off, vals[i]);
	sendto(osc_socket, buf, off, 0, (struct sockaddr*)dst, sizeof(*dst));
}

static void purge_subscribers(time_t now);   /* weiter unten */

/* Snapshot current state, send to subscribers. Called by peak reporter. */
static void broadcast_meters(void) {
	float pin[N_CHANNELS], pout[N_CHANNELS], wts[N_CHANNELS];
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		float p_in_lin  = atomic_exchange(&peak_in_acc[METER_NET][ch],  0.0f);
		float p_out_lin = atomic_exchange(&peak_out_acc[METER_NET][ch], 0.0f);
		pin[ch]  = lin_to_db(p_in_lin);
		pout[ch] = lin_to_db(p_out_lin);
		wts[ch]  = atomic_load_explicit(&weight_acc[ch], memory_order_relaxed);
	}
	float sc_db   = atomic_load_explicit(&current_sc_db,          memory_order_relaxed);
	float gain_db = atomic_load_explicit(&current_gain_atomic_db, memory_order_relaxed);
	float sc_max  = atomic_exchange(&sc_max_acc[METER_NET], -120.0f);

	pthread_mutex_lock(&subscriber_mtx);
	purge_subscribers(time(NULL));
	for (int s = 0; s < subscriber_count; s++) {
		send_eight_floats(&subscribers[s], "/cinecomp/peaks_in",  pin);
		send_eight_floats(&subscribers[s], "/cinecomp/peaks_out", pout);
		send_eight_floats(&subscribers[s], "/cinecomp/weights",   wts);
		send_one_float(&subscribers[s], "/cinecomp/sc_db",   sc_db);
		send_one_float(&subscribers[s], "/cinecomp/gain_db", gain_db);
		send_one_float(&subscribers[s], "/cinecomp/sc_max",  sc_max);
	}
	pthread_mutex_unlock(&subscriber_mtx);
}

static void send_state(struct sockaddr_in *dst) {
	send_one_float(dst, "/cinecomp/threshold",  atomic_load_explicit(&p_threshold_db, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/ratio",      atomic_load_explicit(&p_ratio,        memory_order_relaxed));
	send_one_float(dst, "/cinecomp/attack_ms",  atomic_load_explicit(&p_attack_ms,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/release_ms", atomic_load_explicit(&p_release_ms,   memory_order_relaxed));
	send_one_float(dst, "/cinecomp/hold_ms",    atomic_load_explicit(&p_hold_ms,      memory_order_relaxed));
	send_one_float(dst, "/cinecomp/knee_db",    atomic_load_explicit(&p_knee_db,      memory_order_relaxed));
	send_one_float(dst, "/cinecomp/makeup_db",  atomic_load_explicit(&p_makeup_db,    memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/makeup_follow_dialog",
	               atomic_load_explicit(&p_makeup_follow_dialog, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/wet_dry",    atomic_load_explicit(&p_wet_dry,      memory_order_relaxed));
	send_one_float(dst, "/cinecomp/rms_win_ms", atomic_load_explicit(&p_rms_win_ms,   memory_order_relaxed));
	send_one_float(dst, "/cinecomp/sc_hpf_hz",  atomic_load_explicit(&p_sc_hpf_hz,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/lookahead_ms", atomic_load_explicit(&p_lookahead_ms, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/max_gain",   atomic_load_explicit(&p_max_gain_db,  memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/detector",   engine_get_param_i(PARAM_DETECTOR_MODE));
	send_one_int  (dst, "/cinecomp/duck/center_pct", atomic_load_explicit(&p_duck_center_pct, memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/det_up",     atomic_load_explicit(&p_det_up,   memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/det_down",   atomic_load_explicit(&p_det_down, memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/downward/enable",   atomic_load_explicit(&p_downward_en,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/downward/threshold", atomic_load_explicit(&p_down_threshold, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/downward/ratio",     atomic_load_explicit(&p_down_ratio,     memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/bypass",     atomic_load_explicit(&p_bypass,       memory_order_relaxed));
	send_one_int  (dst, "/cinecomp/preset/active", engine_preset_active());

	/* Zonal-architecture params. */
	send_one_int  (dst, "/cinecomp/architecture",   atomic_load_explicit(&p_architecture_mode, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/atmo/threshold", atomic_load_explicit(&p_atmo_threshold,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/atmo/max_gain",  atomic_load_explicit(&p_atmo_max_gain,     memory_order_relaxed));
	send_one_float(dst, "/cinecomp/atmo/knee",      atomic_load_explicit(&p_atmo_knee,         memory_order_relaxed));
	send_one_float(dst, "/cinecomp/dialog/threshold", atomic_load_explicit(&p_dialog_threshold,  memory_order_relaxed));
	send_one_float(dst, "/cinecomp/dialog/max_gain",  atomic_load_explicit(&p_dialog_max_gain,   memory_order_relaxed));
	send_one_float(dst, "/cinecomp/dialog/knee",      atomic_load_explicit(&p_dialog_knee,       memory_order_relaxed));
	send_one_float(dst, "/cinecomp/noise/floor",    atomic_load_explicit(&p_noise_floor_db,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/noise/knee",     atomic_load_explicit(&p_noise_knee_db,     memory_order_relaxed));
	send_one_float(dst, "/cinecomp/upward/attack_ms",  atomic_load_explicit(&p_upward_attack_ms,  memory_order_relaxed));
	send_one_float(dst, "/cinecomp/upward/release_ms", atomic_load_explicit(&p_upward_release_ms, memory_order_relaxed));
	send_one_float(dst, "/cinecomp/duck/attack_ms",    atomic_load_explicit(&p_duck_attack_ms,    memory_order_relaxed));
	send_one_float(dst, "/cinecomp/duck/release_ms",   atomic_load_explicit(&p_duck_release_ms,   memory_order_relaxed));
}

static int sockaddr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b) {
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* Abgelaufene Abonnenten austragen. Mit gehaltenem Mutex zu rufen. */
static void purge_subscribers(time_t now) {
	for (int i = subscriber_count - 1; i >= 0; i--) {
		if (now - subscriber_seen[i] > SUBSCRIBER_TTL) {
			if (verbose) fprintf(stderr, "subscriber timeout: %s:%d\n",
			                     inet_ntoa(subscribers[i].sin_addr),
			                     ntohs(subscribers[i].sin_port));
			subscribers[i]      = subscribers[subscriber_count - 1];
			subscriber_seen[i]  = subscriber_seen[subscriber_count - 1];
			subscriber_count--;
		}
	}
}

static void subscribe(const struct sockaddr_in *src) {
	time_t now = time(NULL);
	pthread_mutex_lock(&subscriber_mtx);
	for (int i = 0; i < subscriber_count; i++) {
		if (sockaddr_eq(&subscribers[i], src)) {
			subscriber_seen[i] = now;      /* Auffrischen, nicht verdoppeln */
			pthread_mutex_unlock(&subscriber_mtx);
			return;
		}
	}
	purge_subscribers(now);
	if (subscriber_count >= MAX_SUBSCRIBERS) {
		/* Voll und keiner abgelaufen: den aeltesten hergeben. Ein neuer
		 * Abonnent, der gerade fragt, ist mehr wert als einer, der seit
		 * Sekunden schweigt. */
		int oldest = 0;
		for (int i = 1; i < subscriber_count; i++)
			if (subscriber_seen[i] < subscriber_seen[oldest]) oldest = i;
		subscribers[oldest]     = *src;
		subscriber_seen[oldest] = now;
	} else {
		subscriber_seen[subscriber_count] = now;
		subscribers[subscriber_count++]   = *src;
		if (verbose) fprintf(stderr, "subscribe: %s:%d (now %d)\n",
		                     inet_ntoa(src->sin_addr), ntohs(src->sin_port),
		                     subscriber_count);
	}
	pthread_mutex_unlock(&subscriber_mtx);
}

static void unsubscribe(const struct sockaddr_in *src) {
	pthread_mutex_lock(&subscriber_mtx);
	for (int i = 0; i < subscriber_count; i++) {
		if (sockaddr_eq(&subscribers[i], src)) {
			subscribers[i]     = subscribers[subscriber_count - 1];
			subscriber_seen[i] = subscriber_seen[subscriber_count - 1];
			subscriber_count--;
			break;
		}
	}
	pthread_mutex_unlock(&subscriber_mtx);
}

static void handle_osc(const uint8_t *buf, int len, struct sockaddr_in *src) {
	if (len < 8) return;
	char path[128], types[16];
	int off = osc_read_string(buf, len, 0, path, sizeof(path));
	if (off >= len) return;
	off = osc_read_string(buf, len, off, types, sizeof(types));

	/* Helpers to read first float / int argument if present. */
	#define READ_F(default_val) ((off + 4 <= len && types[1] == 'f') ? osc_read_float(buf, off) : (default_val))
	#define READ_I(default_val) ((off + 4 <= len && types[1] == 'i') ? osc_read_int(buf, off) : (default_val))

	if (strcmp(path, "/cinecomp/threshold")  == 0) atomic_store(&p_threshold_db, fclampf(READ_F(0.0f), -60.0f, 0.0f));
	else if (strcmp(path, "/cinecomp/ratio") == 0) atomic_store(&p_ratio,        fclampf(READ_F(3.5f), 1.0f, 20.0f));
	else if (strcmp(path, "/cinecomp/attack_ms")  == 0) atomic_store(&p_attack_ms,  fclampf(READ_F(30.0f), 1.0f, 200.0f));
	else if (strcmp(path, "/cinecomp/release_ms") == 0) atomic_store(&p_release_ms, fclampf(READ_F(400.0f), 10.0f, 2000.0f));
	else if (strcmp(path, "/cinecomp/hold_ms") == 0) atomic_store(&p_hold_ms, fclampf(READ_F(25.0f), 0.0f, 200.0f));
	else if (strcmp(path, "/cinecomp/knee_db") == 0) atomic_store(&p_knee_db, fclampf(READ_F(6.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/makeup_db") == 0) atomic_store(&p_makeup_db, fclampf(READ_F(0.0f), -30.0f, 20.0f));
	else if (strcmp(path, "/cinecomp/makeup_follow_dialog") == 0) atomic_store(&p_makeup_follow_dialog, READ_I(0) ? 1 : 0);
	else if (strcmp(path, "/cinecomp/wet_dry") == 0) atomic_store(&p_wet_dry, fclampf(READ_F(1.0f), 0.0f, 1.0f));
	else if (strcmp(path, "/cinecomp/rms_win_ms") == 0) atomic_store(&p_rms_win_ms, fclampf(READ_F(300.0f), 10.0f, 2000.0f));
	else if (strcmp(path, "/cinecomp/sc_hpf_hz") == 0) atomic_store(&p_sc_hpf_hz, fclampf(READ_F(60.0f), 20.0f, 500.0f));
	else if (strcmp(path, "/cinecomp/lookahead_ms") == 0) atomic_store(&p_lookahead_ms, fclampf(READ_F(5.0f), 0.0f, 20.0f));
	else if (strcmp(path, "/cinecomp/max_gain") == 0) atomic_store(&p_max_gain_db, fclampf(READ_F(12.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/detector") == 0) {
		int v = READ_I(0);
		/* 0=RMS, 1=Peak, 2=Dual (zonal only). Clamp to [0,2].
		 * Setzt beide Richtungen, siehe engine_set_param_i. */
		if (v < 0) v = 0; else if (v > 2) v = 2;
		engine_set_param_i(PARAM_DETECTOR_MODE, v);
	}
	else if (strcmp(path, "/cinecomp/duck/center_pct") == 0) {
		int v = READ_I(100);
		if (v < 0) v = 0; else if (v > 100) v = 100;
		atomic_store(&p_duck_center_pct, v);
	}
	else if (strcmp(path, "/cinecomp/det_up") == 0)
		atomic_store(&p_det_up, READ_I(1) ? 1 : 0);
	else if (strcmp(path, "/cinecomp/det_down") == 0)
		atomic_store(&p_det_down, READ_I(1) ? 1 : 0);
	else if (strcmp(path, "/cinecomp/downward/enable") == 0) atomic_store(&p_downward_en, READ_I(0) ? 1 : 0);
	else if (strcmp(path, "/cinecomp/downward/threshold") == 0) atomic_store(&p_down_threshold, fclampf(READ_F(-6.0f), -60.0f, 0.0f));
	else if (strcmp(path, "/cinecomp/downward/ratio") == 0) atomic_store(&p_down_ratio, fclampf(READ_F(4.0f), 1.0f, 20.0f));
	else if (strcmp(path, "/cinecomp/bypass") == 0) atomic_store(&p_bypass, READ_I(1) ? 1 : 0);
	else if (strcmp(path, "/cinecomp/architecture") == 0) {
		int v = READ_I(0);
		if (v < 0) v = 0; else if (v > 2) v = 2;
		atomic_store(&p_architecture_mode, v);
	}
	else if (strcmp(path, "/cinecomp/atmo/threshold") == 0) atomic_store(&p_atmo_threshold, fclampf(READ_F(-35.0f), -80.0f, 0.0f));
	else if (strcmp(path, "/cinecomp/atmo/max_gain") == 0) atomic_store(&p_atmo_max_gain, fclampf(READ_F(24.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/atmo/knee") == 0) atomic_store(&p_atmo_knee, fclampf(READ_F(20.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/dialog/threshold") == 0) atomic_store(&p_dialog_threshold, fclampf(READ_F(-10.5f), -60.0f, 0.0f));
	else if (strcmp(path, "/cinecomp/dialog/max_gain") == 0) atomic_store(&p_dialog_max_gain, fclampf(READ_F(10.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/dialog/knee") == 0) atomic_store(&p_dialog_knee, fclampf(READ_F(13.5f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/noise/floor") == 0) atomic_store(&p_noise_floor_db, fclampf(READ_F(-70.5f), -90.0f, -20.0f));
	else if (strcmp(path, "/cinecomp/noise/knee") == 0) atomic_store(&p_noise_knee_db, fclampf(READ_F(10.5f), 0.0f, 30.0f));
	else if (strcmp(path, "/cinecomp/upward/attack_ms") == 0) atomic_store(&p_upward_attack_ms, fclampf(READ_F(451.0f), 1.0f, 5000.0f));
	else if (strcmp(path, "/cinecomp/upward/release_ms") == 0) atomic_store(&p_upward_release_ms, fclampf(READ_F(20.0f), 1.0f, 10000.0f));
	else if (strcmp(path, "/cinecomp/duck/attack_ms") == 0) atomic_store(&p_duck_attack_ms, fclampf(READ_F(9.0f), 0.1f, 100.0f));
	else if (strcmp(path, "/cinecomp/duck/release_ms") == 0) atomic_store(&p_duck_release_ms, fclampf(READ_F(51.0f), 1.0f, 2000.0f));
	else if (strcmp(path, "/cinecomp/preset/select") == 0) engine_preset_apply((engine_preset_slot_t)READ_I(1));
	else if (strcmp(path, "/cinecomp/preset/save")   == 0) engine_preset_save((engine_preset_slot_t)READ_I(engine_preset_active() < 0 ? 1 : engine_preset_active()));
	else if (strcmp(path, "/cinecomp/preset/reset")  == 0) engine_preset_reset((engine_preset_slot_t)READ_I(engine_preset_active() < 0 ? 1 : engine_preset_active()));
	/* Named presets: single string arg = preset name. LAN scope. */
	else if (strcmp(path, "/cinecomp/named/save") == 0) {
		char nm[ENGINE_NAME_LEN];
		if (off + 4 <= len && types[1] == 's') {
			osc_read_string(buf, len, off, nm, sizeof(nm));
			engine_named_save(nm);
		}
	}
	else if (strcmp(path, "/cinecomp/named/apply") == 0) {
		char nm[ENGINE_NAME_LEN];
		if (off + 4 <= len && types[1] == 's') {
			osc_read_string(buf, len, off, nm, sizeof(nm));
			engine_named_apply(nm);
		}
	}
	else if (strcmp(path, "/cinecomp/named/delete") == 0) {
		char nm[ENGINE_NAME_LEN];
		if (off + 4 <= len && types[1] == 's') {
			osc_read_string(buf, len, off, nm, sizeof(nm));
			engine_named_delete(nm);
		}
	}
	else if (strcmp(path, "/cinecomp/subscribe") == 0) subscribe(src);
	else if (strcmp(path, "/cinecomp/unsubscribe") == 0) unsubscribe(src);
	else if (strcmp(path, "/cinecomp/get") == 0) send_state(src);
	else if (verbose) fprintf(stderr, "osc: unhandled path %s (types %s)\n", path, types);

	#undef READ_F
	#undef READ_I
}

static void *osc_server(void *arg) {
	(void)arg;
	while (running) {
		uint8_t buf[OSC_BUFSIZE];
		struct sockaddr_in src;
		socklen_t srclen = sizeof(src);
		int n = recvfrom(osc_socket, buf, sizeof(buf), 0,
		                 (struct sockaddr*)&src, &srclen);
		if (n > 0) handle_osc(buf, n, &src);
	}
	return NULL;
}

static void *peak_reporter(void *arg) {
	(void)arg;
	/* 50 ms = 20 Hz broadcast. */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
	while (running) {
		nanosleep(&ts, NULL);
		broadcast_meters();
	}
	return NULL;
}

/* ----------------------------- Lifecycle -------------------------------- */

static const char *channel_names[N_CHANNELS] = {
	"L", "R", "C", "LFE", "LS", "RS", "RBL", "RBR"
};

#ifndef CINECOMP_NO_JACK
static int jack_register_ports(void) {
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		char name[32];
		snprintf(name, sizeof(name), "in_%s", channel_names[ch]);
		in_ports[ch] = jack_port_register(client, name,
			JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		if (!in_ports[ch]) return -1;

		snprintf(name, sizeof(name), "out_%s", channel_names[ch]);
		out_ports[ch] = jack_port_register(client, name,
			JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (!out_ports[ch]) return -1;
	}
	return 0;
}

/* ----------------------------- Public engine API --------------------- */

static pthread_t osc_th, peak_th;
static int engine_running = 0;

int engine_start(const char *name, int port) {
	if (engine_running) return 0;
	if (name && *name) jack_name = name;
	if (port > 0)      osc_port  = port;
	running = 1;

	osc_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (osc_socket < 0) { perror("socket"); return -1; }
	/* SO_RCVTIMEO so recvfrom() returns periodically and the OSC thread
	 * can observe `running == 0` on engine_stop(). Without this, close()
	 * is not guaranteed to wake a blocked recvfrom in glibc/Linux and the
	 * process hangs on pthread_join during shutdown. */
	struct timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 200000 };
	setsockopt(osc_socket, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
	/* Bind to INADDR_ANY so LAN clients (cycle script on amos via LIRC,
	 * other remote control) can drive presets. Personal-LAN scope; if
	 * this ever runs on a multi-tenant host, an explicit allow-list
	 * would be needed. */
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
		.sin_port = htons(osc_port),
	};
	if (bind(osc_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind"); return -1;
	}

	jack_status_t status;
	client = jack_client_open(jack_name, JackNoStartServer, &status);
	if (!client) {
		fprintf(stderr, "jack_client_open failed (status 0x%x)\n", status);
		return -1;
	}

	sample_rate = jack_get_sample_rate(client);

	if (jack_register_ports() < 0) {
		fprintf(stderr, "port registration failed\n");
		return -1;
	}

	jack_set_process_callback(client, audio_callback, NULL);
	jack_set_sample_rate_callback(client, sample_rate_callback, NULL);

	update_hpf_coeffs(20.0f, (float)sample_rate);
	current_hpf_hz = 20.0f;

	if (jack_activate(client) < 0) {
		fprintf(stderr, "jack_activate failed\n");
		return -1;
	}

	pthread_create(&osc_th,  NULL, osc_server,    NULL);
	pthread_create(&peak_th, NULL, peak_reporter, NULL);
	engine_running = 1;
	return 0;
}

void engine_stop(void) {
	if (!engine_running) return;
	running = 0;

	/* Wake the OSC server thread out of recvfrom() immediately by sending
	 * a dummy packet to our own bound port. SO_RCVTIMEO alone has shown
	 * to occasionally not fire reliably on some kernels; this guarantees
	 * wakeup. */
	if (osc_socket >= 0) {
		int wake = socket(AF_INET, SOCK_DGRAM, 0);
		if (wake >= 0) {
			struct sockaddr_in dst = {
				.sin_family = AF_INET,
				.sin_addr   = { .s_addr = htonl(INADDR_LOOPBACK) },
				.sin_port   = htons(0),
			};
			/* Read our own bound port and reuse it as dst */
			socklen_t len = sizeof(dst);
			if (getsockname(osc_socket, (struct sockaddr*)&dst, &len) == 0) {
				char b = 0;
				sendto(wake, &b, 1, 0, (struct sockaddr*)&dst, sizeof(dst));
			}
			close(wake);
		}
	}

	if (client) {
		jack_deactivate(client);
		jack_client_close(client);
		client = NULL;
	}
	if (osc_socket >= 0) {
		shutdown(osc_socket, SHUT_RDWR);
		close(osc_socket);
		osc_socket = -1;
	}
	/* pthread_timedjoin_np lets us exit even if a thread is stuck
	 * (e.g. JACK quirk or PipeWire-JACK shutdown delay). 500 ms is
	 * generous given recvfrom timeout is 200 ms. */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 500 * 1000 * 1000;
	if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
	if (pthread_timedjoin_np(osc_th,  NULL, &ts) != 0)
		pthread_cancel(osc_th);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 200 * 1000 * 1000;
	if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
	if (pthread_timedjoin_np(peak_th, NULL, &ts) != 0)
		pthread_cancel(peak_th);

	engine_running = 0;
}
#endif /* CINECOMP_NO_JACK */

unsigned engine_sample_rate(void) {
	return sample_rate;
}

/* Param-table dispatch: each enum maps to one atomic. Done with two
 * switches (one per type) to keep this branch-predictor-friendly. */
float engine_get_param_f(engine_param_t id) {
	switch (id) {
	case PARAM_THRESHOLD:        return atomic_load(&p_threshold_db);
	case PARAM_RATIO:            return atomic_load(&p_ratio);
	case PARAM_ATTACK_MS:        return atomic_load(&p_attack_ms);
	case PARAM_RELEASE_MS:       return atomic_load(&p_release_ms);
	case PARAM_HOLD_MS:          return atomic_load(&p_hold_ms);
	case PARAM_KNEE_DB:          return atomic_load(&p_knee_db);
	case PARAM_MAKEUP_DB:        return atomic_load(&p_makeup_db);
	case PARAM_WET_DRY:          return atomic_load(&p_wet_dry);
	case PARAM_RMS_WIN_MS:       return atomic_load(&p_rms_win_ms);
	case PARAM_SC_HPF_HZ:        return atomic_load(&p_sc_hpf_hz);
	case PARAM_LOOKAHEAD_MS:     return atomic_load(&p_lookahead_ms);
	case PARAM_MAX_GAIN_DB:      return atomic_load(&p_max_gain_db);
	case PARAM_DOWN_THRESHOLD:   return atomic_load(&p_down_threshold);
	case PARAM_DOWN_RATIO:       return atomic_load(&p_down_ratio);
	case PARAM_ATMO_THRESHOLD:   return atomic_load(&p_atmo_threshold);
	case PARAM_ATMO_MAX_GAIN:    return atomic_load(&p_atmo_max_gain);
	case PARAM_ATMO_KNEE:        return atomic_load(&p_atmo_knee);
	case PARAM_DIALOG_THRESHOLD: return atomic_load(&p_dialog_threshold);
	case PARAM_DIALOG_MAX_GAIN:  return atomic_load(&p_dialog_max_gain);
	case PARAM_DIALOG_KNEE:      return atomic_load(&p_dialog_knee);
	case PARAM_NOISE_FLOOR_DB:   return atomic_load(&p_noise_floor_db);
	case PARAM_NOISE_KNEE_DB:    return atomic_load(&p_noise_knee_db);
	case PARAM_UPWARD_ATTACK_MS: return atomic_load(&p_upward_attack_ms);
	case PARAM_UPWARD_RELEASE_MS:return atomic_load(&p_upward_release_ms);
	case PARAM_DUCK_ATTACK_MS:   return atomic_load(&p_duck_attack_ms);
	case PARAM_DUCK_RELEASE_MS:  return atomic_load(&p_duck_release_ms);
	default: return 0.0f;
	}
}

void engine_set_param_f(engine_param_t id, float v) {
	switch (id) {
	case PARAM_THRESHOLD:        atomic_store(&p_threshold_db, fclampf(v, -60.0f, 0.0f)); break;
	case PARAM_RATIO:            atomic_store(&p_ratio,        fclampf(v, 1.0f, 20.0f)); break;
	case PARAM_ATTACK_MS:        atomic_store(&p_attack_ms,    fclampf(v, 1.0f, 200.0f)); break;
	case PARAM_RELEASE_MS:       atomic_store(&p_release_ms,   fclampf(v, 10.0f, 2000.0f)); break;
	case PARAM_HOLD_MS:          atomic_store(&p_hold_ms,      fclampf(v, 0.0f, 200.0f)); break;
	case PARAM_KNEE_DB:          atomic_store(&p_knee_db,      fclampf(v, 0.0f, 30.0f)); break;
	/* Nach unten so weit, wie der Dialog-Lift nach oben kann (30 dB) - sonst
	 * laesst sich sein Maximum von Hand nicht mehr ausgleichen, und die
	 * Kopplung koennte an einer Klemme haengen bleiben, die sie nicht sieht. */
	case PARAM_MAKEUP_DB:        atomic_store(&p_makeup_db,    fclampf(v, -30.0f, 20.0f)); break;
	case PARAM_WET_DRY:          atomic_store(&p_wet_dry,      fclampf(v, 0.0f, 1.0f)); break;
	case PARAM_RMS_WIN_MS:       atomic_store(&p_rms_win_ms,   fclampf(v, 10.0f, 2000.0f)); break;
	case PARAM_SC_HPF_HZ:        atomic_store(&p_sc_hpf_hz,    fclampf(v, 20.0f, 500.0f)); break;
	case PARAM_LOOKAHEAD_MS:     atomic_store(&p_lookahead_ms, fclampf(v, 0.0f, 20.0f)); break;
	case PARAM_MAX_GAIN_DB:      atomic_store(&p_max_gain_db,  fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_DOWN_THRESHOLD:   atomic_store(&p_down_threshold, fclampf(v, -60.0f, 0.0f)); break;
	case PARAM_DOWN_RATIO:       atomic_store(&p_down_ratio,   fclampf(v, 1.0f, 20.0f)); break;
	case PARAM_ATMO_THRESHOLD:   atomic_store(&p_atmo_threshold, fclampf(v, -80.0f, 0.0f)); break;
	case PARAM_ATMO_MAX_GAIN:    atomic_store(&p_atmo_max_gain,  fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_ATMO_KNEE:        atomic_store(&p_atmo_knee,      fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_DIALOG_THRESHOLD: atomic_store(&p_dialog_threshold, fclampf(v, -60.0f, 0.0f)); break;
	case PARAM_DIALOG_MAX_GAIN:  atomic_store(&p_dialog_max_gain,  fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_DIALOG_KNEE:      atomic_store(&p_dialog_knee,      fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_NOISE_FLOOR_DB:   atomic_store(&p_noise_floor_db,   fclampf(v, -90.0f, -20.0f)); break;
	case PARAM_NOISE_KNEE_DB:    atomic_store(&p_noise_knee_db,    fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_UPWARD_ATTACK_MS: atomic_store(&p_upward_attack_ms,  fclampf(v, 1.0f, 5000.0f)); break;
	case PARAM_UPWARD_RELEASE_MS:atomic_store(&p_upward_release_ms, fclampf(v, 1.0f, 10000.0f)); break;
	case PARAM_DUCK_ATTACK_MS:   atomic_store(&p_duck_attack_ms,    fclampf(v, 0.1f, 100.0f)); break;
	case PARAM_DUCK_RELEASE_MS:  atomic_store(&p_duck_release_ms,   fclampf(v, 1.0f, 2000.0f)); break;
	default: break;
	}
}

int engine_get_param_i(engine_param_t id) {
	switch (id) {
	case PARAM_MAKEUP_FOLLOW_DIALOG: return atomic_load(&p_makeup_follow_dialog);
	case PARAM_DETECTOR_MODE: {
		/* Abgeleitet, damit alte Leser (OSC, Presets) etwas Sinnvolles
		 * sehen. Peak oben / RMS unten hat keine alte Entsprechung und
		 * meldet sich als Peak - fuer diese Kombination gibt es
		 * PARAM_DET_UP und PARAM_DET_DOWN. */
		int u = atomic_load(&p_det_up), d = atomic_load(&p_det_down);
		if (u == 0 && d == 0) return 0;
		if (u == 1 && d == 1) return 1;
		if (u == 0 && d == 1) return 2;
		return 1;
	}
	case PARAM_DUCK_CENTER_PCT:   return atomic_load(&p_duck_center_pct);
	case PARAM_DET_UP:            return atomic_load(&p_det_up);
	case PARAM_DET_DOWN:          return atomic_load(&p_det_down);
	case PARAM_DOWNWARD_EN:       return atomic_load(&p_downward_en);
	case PARAM_BYPASS:            return atomic_load(&p_bypass);
	case PARAM_ARCHITECTURE_MODE: return atomic_load(&p_architecture_mode);
	default: return 0;
	}
}

void engine_set_param_i(engine_param_t id, int v) {
	switch (id) {
	case PARAM_DETECTOR_MODE: {
		/* Setzt beide Richtungen auf einmal - das ist der Weg, auf dem
		 * alte Presets und alte OSC-Sender weiter ankommen. */
		int x = v;
		if (x < 0) x = 0; else if (x > 2) x = 2;
		atomic_store(&p_detector_mode, x);
		atomic_store(&p_det_up,   (x == 1) ? 1 : 0);
		atomic_store(&p_det_down, (x == 0) ? 0 : 1);
		break;
	}
	case PARAM_DUCK_CENTER_PCT: {
		int x = v; if (x < 0) x = 0; else if (x > 100) x = 100;
		atomic_store(&p_duck_center_pct, x); break;
	}
	case PARAM_DET_UP:   atomic_store(&p_det_up,   v ? 1 : 0); break;
	case PARAM_DET_DOWN: atomic_store(&p_det_down, v ? 1 : 0); break;
	case PARAM_DOWNWARD_EN:       atomic_store(&p_downward_en, v ? 1 : 0); break;
	case PARAM_BYPASS:            atomic_store(&p_bypass,      v ? 1 : 0); break;
	case PARAM_MAKEUP_FOLLOW_DIALOG:
		atomic_store(&p_makeup_follow_dialog, v ? 1 : 0); break;
	case PARAM_ARCHITECTURE_MODE: {
		int x = v;
		if (x < 0) x = 0; else if (x > 2) x = 2;
		atomic_store(&p_architecture_mode, x);
		break;
	}
	default: break;
	}
}

void engine_read_meters(engine_meters_t *out) {
	if (!out) return;
	for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
		float p_in  = atomic_exchange(&peak_in_acc[METER_LOCAL][ch],  0.0f);
		float p_out = atomic_exchange(&peak_out_acc[METER_LOCAL][ch], 0.0f);
		out->peak_in[ch]  = lin_to_db(p_in);
		out->peak_out[ch] = lin_to_db(p_out);
		out->weight[ch]   = atomic_load_explicit(&weight_acc[ch], memory_order_relaxed);
	}
	out->sc_db   = atomic_load_explicit(&current_sc_db,           memory_order_relaxed);
	out->gain_db = atomic_load_explicit(&current_gain_atomic_db,  memory_order_relaxed);
	out->sc_max  = atomic_exchange(&sc_max_acc[METER_LOCAL], -120.0f);
	out->sc_dn_db = atomic_load_explicit(&current_sc_dn_db, memory_order_relaxed);
}

/* ----------------------- Presets ---------------------------------------
 * Three named slots. Factory defaults mirror the aroio6 Buildroot package
 * `state.go defaultFilmcompPreset()` cinema-tuning. MID is the boot
 * default (Wecker-getestet + zonal-v2 plateau, 2026-05-15). All three
 * use zonal-v2 architecture + Peak detector. Classic/v1 are retired and
 * no longer reachable from the GUI; the classic-stage params are kept
 * only so the OSC API and old state files stay loadable. */
static const engine_preset_t factory_presets[PRESET__COUNT] = {
	[PRESET_LOW] = {
		.f = {
			[PARAM_THRESHOLD]    =   0.0f,
			[PARAM_RATIO]        =  3.6f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 56.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      = 16.5f,
			[PARAM_MAKEUP_DB]    = -10.3f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 33.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 16.3f,
			[PARAM_DOWN_THRESHOLD] = -6.0f,
			[PARAM_DOWN_RATIO]   =  2.5f,
			/* Zonal — shared across all three preset slots */
			[PARAM_ATMO_THRESHOLD]   = -35.0f,
			[PARAM_ATMO_MAX_GAIN]    =  24.0f,
			[PARAM_ATMO_KNEE]        =  20.0f,
			[PARAM_DIALOG_THRESHOLD] = -10.5f,
			[PARAM_DIALOG_MAX_GAIN]  =  10.0f,
			[PARAM_DIALOG_KNEE]      =  13.5f,
			[PARAM_NOISE_FLOOR_DB]   = -80.0f,
			[PARAM_NOISE_KNEE_DB]    =  10.5f,
			[PARAM_UPWARD_ATTACK_MS] = 451.0f,
			[PARAM_UPWARD_RELEASE_MS]=  20.0f,
			[PARAM_DUCK_ATTACK_MS]   =   9.0f,
			[PARAM_DUCK_RELEASE_MS]  =  51.0f,
		},
		.detector_mode     = 1, .det_up = -1, .det_down = -1,
		.downward_en       = 0,
		.architecture_mode = 2,
	},
	[PRESET_MID] = {
		.f = {
			[PARAM_THRESHOLD]    = -10.0f,
			[PARAM_RATIO]        =  4.0f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 20.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      = 16.5f,
			[PARAM_MAKEUP_DB]    =  0.0f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 60.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 10.0f,
			[PARAM_DOWN_THRESHOLD] = -6.0f,
			[PARAM_DOWN_RATIO]   =  4.0f,
			[PARAM_ATMO_THRESHOLD]   = -35.0f,
			[PARAM_ATMO_MAX_GAIN]    =  24.0f,
			[PARAM_ATMO_KNEE]        =  20.0f,
			[PARAM_DIALOG_THRESHOLD] = -10.5f,
			[PARAM_DIALOG_MAX_GAIN]  =  10.0f,
			[PARAM_DIALOG_KNEE]      =  13.5f,
			[PARAM_NOISE_FLOOR_DB]   = -80.0f,
			[PARAM_NOISE_KNEE_DB]    =  10.5f,
			[PARAM_UPWARD_ATTACK_MS] = 451.0f,
			[PARAM_UPWARD_RELEASE_MS]=  20.0f,
			[PARAM_DUCK_ATTACK_MS]   =   9.0f,
			[PARAM_DUCK_RELEASE_MS]  =  51.0f,
		},
		.detector_mode     = 1, .det_up = -1, .det_down = -1,
		.downward_en       = 1,
		.architecture_mode = 2,
	},
	[PRESET_HIGH] = {
		.f = {
			[PARAM_THRESHOLD]    = -10.0f,
			[PARAM_RATIO]        =  6.0f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 800.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      = 16.5f,
			[PARAM_MAKEUP_DB]    =  0.0f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 50.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 18.5f,
			[PARAM_DOWN_THRESHOLD] = -14.0f,
			[PARAM_DOWN_RATIO]   =  1.8f,
			[PARAM_ATMO_THRESHOLD]   = -35.0f,
			[PARAM_ATMO_MAX_GAIN]    =  24.0f,
			[PARAM_ATMO_KNEE]        =  20.0f,
			[PARAM_DIALOG_THRESHOLD] = -10.5f,
			[PARAM_DIALOG_MAX_GAIN]  =  10.0f,
			[PARAM_DIALOG_KNEE]      =  13.5f,
			[PARAM_NOISE_FLOOR_DB]   = -80.0f,
			[PARAM_NOISE_KNEE_DB]    =  10.5f,
			[PARAM_UPWARD_ATTACK_MS] = 451.0f,
			[PARAM_UPWARD_RELEASE_MS]=  20.0f,
			[PARAM_DUCK_ATTACK_MS]   =   9.0f,
			[PARAM_DUCK_RELEASE_MS]  =  51.0f,
		},
		.detector_mode     = 1, .det_up = -1, .det_down = -1,
		.downward_en       = 1,
		.architecture_mode = 2,
	},
};

/* User-editable copies. Initially identical to factory; survive across
 * runs via state.ini persistence. */
static engine_preset_t presets[PRESET__COUNT];
static pthread_mutex_t  preset_mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int      active_preset_slot = -1;
static int              presets_initialised = 0;

static void preset_init_once(void) {
	if (presets_initialised) return;
	for (int i = 0; i < PRESET__COUNT; i++) presets[i] = factory_presets[i];
	presets_initialised = 1;
}

static const char *preset_names[PRESET__COUNT] = { "low", "mid", "high" };

const char *engine_preset_name(engine_preset_slot_t slot) {
	if (slot < 0 || slot >= PRESET__COUNT) return "?";
	return preset_names[slot];
}

/* backfill_zonal_defaults — pre-zonal artifact repair. A preset written by
 * a pre-zonal binary always has architecture_mode==0 AND atmo_max_gain==0
 * AND dialog_max_gain==0 (zonal fields never written). That's an artifact,
 * not a deliberate Classic choice — restore the zonal architecture + zonal
 * fields from the MID factory preset so applying it doesn't drag the UI
 * into Classic with zeroed zonal params. Mirrors backfillZonalDefaults*()
 * in the aroio6 backend (state.go). */
static void backfill_zonal_defaults(engine_preset_t *p) {
	if (!p) return;
	if (p->f[PARAM_ATMO_MAX_GAIN] != 0.0f ||
	    p->f[PARAM_DIALOG_MAX_GAIN] != 0.0f)
		return;
	const engine_preset_t *d = &factory_presets[PRESET_MID];
	p->architecture_mode             = d->architecture_mode;
	p->f[PARAM_ATMO_THRESHOLD]       = d->f[PARAM_ATMO_THRESHOLD];
	p->f[PARAM_ATMO_MAX_GAIN]        = d->f[PARAM_ATMO_MAX_GAIN];
	p->f[PARAM_ATMO_KNEE]            = d->f[PARAM_ATMO_KNEE];
	p->f[PARAM_DIALOG_THRESHOLD]     = d->f[PARAM_DIALOG_THRESHOLD];
	p->f[PARAM_DIALOG_MAX_GAIN]      = d->f[PARAM_DIALOG_MAX_GAIN];
	p->f[PARAM_DIALOG_KNEE]          = d->f[PARAM_DIALOG_KNEE];
	p->f[PARAM_NOISE_FLOOR_DB]       = d->f[PARAM_NOISE_FLOOR_DB];
	p->f[PARAM_NOISE_KNEE_DB]        = d->f[PARAM_NOISE_KNEE_DB];
	p->f[PARAM_UPWARD_ATTACK_MS]     = d->f[PARAM_UPWARD_ATTACK_MS];
	p->f[PARAM_UPWARD_RELEASE_MS]    = d->f[PARAM_UPWARD_RELEASE_MS];
	p->f[PARAM_DUCK_ATTACK_MS]       = d->f[PARAM_DUCK_ATTACK_MS];
	p->f[PARAM_DUCK_RELEASE_MS]      = d->f[PARAM_DUCK_RELEASE_MS];
}

/* Push a snapshot into the live atomic params. Shared by slot- and
 * named-preset apply. */
static void preset_apply_live(const engine_preset_t *p) {
	for (int i = 0; i < PARAM__FLOAT_COUNT; i++) {
		engine_set_param_f((engine_param_t)i, p->f[i]);
	}
	/* Reihenfolge zaehlt: der Sammelschalter setzt beide Richtungen, die
	 * beiden Einzelwerte ueberschreiben ihn danach. Ein altes Preset ohne
	 * die neuen Felder traegt dort -1 und laesst den Sammelschalter
	 * stehen. */
	engine_set_param_i(PARAM_DETECTOR_MODE,     p->detector_mode);
	if (p->det_up   >= 0) engine_set_param_i(PARAM_DET_UP,   p->det_up);
	if (p->det_down >= 0) engine_set_param_i(PARAM_DET_DOWN, p->det_down);
	engine_set_param_i(PARAM_DOWNWARD_EN,       p->downward_en);
	engine_set_param_i(PARAM_ARCHITECTURE_MODE, p->architecture_mode);
}

int engine_preset_apply(engine_preset_slot_t slot) {
	if (slot < 0 || slot >= PRESET__COUNT) return -1;
	preset_init_once();
	pthread_mutex_lock(&preset_mtx);
	engine_preset_t p = presets[slot];
	pthread_mutex_unlock(&preset_mtx);

	backfill_zonal_defaults(&p);
	preset_apply_live(&p);
	engine_named_set_active("");          /* slot + named are exclusive */
	atomic_store(&active_preset_slot, (int)slot);
	return 0;
}

void engine_preset_save(engine_preset_slot_t slot) {
	if (slot < 0 || slot >= PRESET__COUNT) return;
	preset_init_once();
	engine_preset_t p;
	for (int i = 0; i < PARAM__FLOAT_COUNT; i++) {
		p.f[i] = engine_get_param_f((engine_param_t)i);
	}
	p.detector_mode     = engine_get_param_i(PARAM_DETECTOR_MODE);
	p.det_up            = engine_get_param_i(PARAM_DET_UP);
	p.det_down          = engine_get_param_i(PARAM_DET_DOWN);
	p.downward_en       = engine_get_param_i(PARAM_DOWNWARD_EN);
	p.architecture_mode = engine_get_param_i(PARAM_ARCHITECTURE_MODE);

	pthread_mutex_lock(&preset_mtx);
	presets[slot] = p;
	pthread_mutex_unlock(&preset_mtx);
}

void engine_preset_reset(engine_preset_slot_t slot) {
	if (slot < 0 || slot >= PRESET__COUNT) return;
	preset_init_once();
	pthread_mutex_lock(&preset_mtx);
	presets[slot] = factory_presets[slot];
	pthread_mutex_unlock(&preset_mtx);
}

int engine_preset_active(void) {
	return atomic_load(&active_preset_slot);
}

void engine_preset_get(engine_preset_slot_t slot, engine_preset_t *out) {
	if (!out || slot < 0 || slot >= PRESET__COUNT) return;
	preset_init_once();
	pthread_mutex_lock(&preset_mtx);
	*out = presets[slot];
	pthread_mutex_unlock(&preset_mtx);
}

void engine_preset_set(engine_preset_slot_t slot, const engine_preset_t *in) {
	if (!in || slot < 0 || slot >= PRESET__COUNT) return;
	preset_init_once();
	pthread_mutex_lock(&preset_mtx);
	presets[slot] = *in;
	pthread_mutex_unlock(&preset_mtx);
}

/* ----------------------- Named presets --------------------------------- */
/* Fixed-capacity array (C has no map). Kept sorted by name so the GUI
 * dropdown order is stable. Guarded by named_mtx (sibling of preset_mtx).
 * named_active mirrors the aroio6 backend's FilmcompActiveName. */

typedef struct {
	char            name[ENGINE_NAME_LEN];
	engine_preset_t p;
} named_preset_t;

static named_preset_t  named_presets[ENGINE_NAMED_MAX];
static int             named_count_v = 0;
static char            named_active[ENGINE_NAME_LEN] = "";
static pthread_mutex_t named_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Linear search; returns index or -1. Caller holds named_mtx. */
static int named_find_locked(const char *name) {
	for (int i = 0; i < named_count_v; i++) {
		if (strcmp(named_presets[i].name, name) == 0) return i;
	}
	return -1;
}

/* Insertion sort by name. Caller holds named_mtx. */
static void named_sort_locked(void) {
	for (int i = 1; i < named_count_v; i++) {
		named_preset_t tmp = named_presets[i];
		int j = i - 1;
		while (j >= 0 && strcmp(named_presets[j].name, tmp.name) > 0) {
			named_presets[j + 1] = named_presets[j];
			j--;
		}
		named_presets[j + 1] = tmp;
	}
}

int engine_named_count(void) {
	pthread_mutex_lock(&named_mtx);
	int n = named_count_v;
	pthread_mutex_unlock(&named_mtx);
	return n;
}

const char *engine_named_name(int idx) {
	/* Returned pointer stays valid until the next mutating call; the GUI
	 * copies it immediately into combo entries, so this is fine for the
	 * single-threaded ImGui draw path. */
	static char buf[ENGINE_NAME_LEN];
	pthread_mutex_lock(&named_mtx);
	if (idx < 0 || idx >= named_count_v) {
		pthread_mutex_unlock(&named_mtx);
		return "";
	}
	snprintf(buf, sizeof(buf), "%s", named_presets[idx].name);
	pthread_mutex_unlock(&named_mtx);
	return buf;
}

int engine_named_get(const char *name, engine_preset_t *out) {
	if (!name || !*name || !out) return -1;
	pthread_mutex_lock(&named_mtx);
	int i = named_find_locked(name);
	if (i < 0) { pthread_mutex_unlock(&named_mtx); return -1; }
	*out = named_presets[i].p;
	pthread_mutex_unlock(&named_mtx);
	backfill_zonal_defaults(out);
	return 0;
}

int engine_named_save(const char *name) {
	if (!name || !*name) return -1;
	preset_init_once();

	/* Snapshot the current live params (same logic as engine_preset_save). */
	engine_preset_t p;
	for (int i = 0; i < PARAM__FLOAT_COUNT; i++) {
		p.f[i] = engine_get_param_f((engine_param_t)i);
	}
	p.detector_mode     = engine_get_param_i(PARAM_DETECTOR_MODE);
	p.det_up            = engine_get_param_i(PARAM_DET_UP);
	p.det_down          = engine_get_param_i(PARAM_DET_DOWN);
	p.downward_en       = engine_get_param_i(PARAM_DOWNWARD_EN);
	p.architecture_mode = engine_get_param_i(PARAM_ARCHITECTURE_MODE);

	pthread_mutex_lock(&named_mtx);
	int i = named_find_locked(name);
	if (i >= 0) {
		named_presets[i].p = p;                 /* overwrite */
	} else {
		if (named_count_v >= ENGINE_NAMED_MAX) {
			pthread_mutex_unlock(&named_mtx);
			return -1;                          /* library full */
		}
		i = named_count_v++;
		snprintf(named_presets[i].name, ENGINE_NAME_LEN, "%s", name);
		named_presets[i].p = p;
		named_sort_locked();
	}
	snprintf(named_active, ENGINE_NAME_LEN, "%s", name);
	pthread_mutex_unlock(&named_mtx);
	atomic_store(&active_preset_slot, -1);      /* slot + named exclusive */
	return 0;
}

void engine_named_set(const char *name, const engine_preset_t *in) {
	if (!name || !*name || !in) return;
	pthread_mutex_lock(&named_mtx);
	int i = named_find_locked(name);
	if (i >= 0) {
		named_presets[i].p = *in;               /* overwrite */
	} else {
		if (named_count_v >= ENGINE_NAMED_MAX) {
			pthread_mutex_unlock(&named_mtx);
			return;                             /* library full */
		}
		i = named_count_v++;
		snprintf(named_presets[i].name, ENGINE_NAME_LEN, "%s", name);
		named_presets[i].p = *in;
		named_sort_locked();
	}
	pthread_mutex_unlock(&named_mtx);
}

/* ---- Built-in stock named presets ------------------------------------
 * Two era families x three intensities, cinema-tuned by Nicola
 * (2026-05-17) and sanity-checked. The era axis lives in the zone map:
 *   90ies  — louder atmo (atmo_thr -39.2) + hotter dialogue (dlg_thr
 *            ~-7.7), smaller perceived spread.
 *   Modern — quieter atmo (atmo_thr -45.3) + lower dialogue (-10.5),
 *            zones spread far apart.
 * The Low/Mid/Hi ladder is a pure intensity axis: atmo lift 8/12/20,
 * dialog lift 4/8/10, makeup -4/-8/-10, duck off/on/on. All v2 + Peak.
 * noise_floor is normalised to the fixed -80 guard. Seeded only when a
 * name is absent (engine_named_seed_factory), so a user's own state.ini
 * copy always wins. Names are kept verbatim; the dropdown sorts them
 * alphabetically (… Hi, … Low, … Mid). */
static const named_preset_t factory_named[] = {
	{ "90ies Low", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-4, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=50,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-6.7f, [PARAM_DOWN_RATIO]=2.1f,
		[PARAM_ATMO_THRESHOLD]=-39.2f, [PARAM_ATMO_MAX_GAIN]=8, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-7.6f, [PARAM_DIALOG_MAX_GAIN]=4, [PARAM_DIALOG_KNEE]=20.2f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=0, .architecture_mode=2 } },
	{ "90ies Mid", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-7.8f, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=50,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-6.7f, [PARAM_DOWN_RATIO]=2.1f,
		[PARAM_ATMO_THRESHOLD]=-39.2f, [PARAM_ATMO_MAX_GAIN]=12, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-7.6f, [PARAM_DIALOG_MAX_GAIN]=8, [PARAM_DIALOG_KNEE]=20.2f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=1, .architecture_mode=2 } },
	{ "90ies Hi", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-10, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=54,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-8, [PARAM_DOWN_RATIO]=2.1f,
		[PARAM_ATMO_THRESHOLD]=-39.2f, [PARAM_ATMO_MAX_GAIN]=20, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-7.9f, [PARAM_DIALOG_MAX_GAIN]=10, [PARAM_DIALOG_KNEE]=12.3f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=1, .architecture_mode=2 } },
	{ "Modern Low", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-4, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=54,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-8, [PARAM_DOWN_RATIO]=1.4f,
		[PARAM_ATMO_THRESHOLD]=-45.3f, [PARAM_ATMO_MAX_GAIN]=8, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-10.5f, [PARAM_DIALOG_MAX_GAIN]=4, [PARAM_DIALOG_KNEE]=12.3f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=0, .architecture_mode=2 } },
	{ "Modern Mid", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-8, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=54,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-8, [PARAM_DOWN_RATIO]=1.4f,
		[PARAM_ATMO_THRESHOLD]=-45.3f, [PARAM_ATMO_MAX_GAIN]=12, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-10.5f, [PARAM_DIALOG_MAX_GAIN]=8, [PARAM_DIALOG_KNEE]=12.3f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=1, .architecture_mode=2 } },
	{ "Modern Hi", { .f = {
		[PARAM_THRESHOLD]=-10, [PARAM_RATIO]=6, [PARAM_ATTACK_MS]=5,
		[PARAM_RELEASE_MS]=800, [PARAM_HOLD_MS]=0, [PARAM_KNEE_DB]=16.5f,
		[PARAM_MAKEUP_DB]=-10, [PARAM_WET_DRY]=1, [PARAM_RMS_WIN_MS]=54,
		[PARAM_SC_HPF_HZ]=20, [PARAM_LOOKAHEAD_MS]=20, [PARAM_MAX_GAIN_DB]=18.5f,
		[PARAM_DOWN_THRESHOLD]=-8, [PARAM_DOWN_RATIO]=2.1f,
		[PARAM_ATMO_THRESHOLD]=-45.3f, [PARAM_ATMO_MAX_GAIN]=20, [PARAM_ATMO_KNEE]=14.9f,
		[PARAM_DIALOG_THRESHOLD]=-10.5f, [PARAM_DIALOG_MAX_GAIN]=10, [PARAM_DIALOG_KNEE]=12.3f,
		[PARAM_NOISE_FLOOR_DB]=-80, [PARAM_NOISE_KNEE_DB]=10.5f,
		[PARAM_UPWARD_ATTACK_MS]=451, [PARAM_UPWARD_RELEASE_MS]=20,
		[PARAM_DUCK_ATTACK_MS]=9, [PARAM_DUCK_RELEASE_MS]=51,
	}, .detector_mode=1, .det_up=-1, .det_down=-1, .downward_en=1, .architecture_mode=2 } },
};

/* Install the built-in stock presets. Only inserts a name that is not
 * already present, so this is idempotent and never clobbers a user's
 * own copy (call before state_load to get factory-default semantics:
 * stock seeded first, the user's state.ini then overrides). */
void engine_named_seed_factory(void) {
	size_t n = sizeof(factory_named) / sizeof(factory_named[0]);
	for (size_t i = 0; i < n; i++) {
		engine_preset_t tmp;
		if (engine_named_get(factory_named[i].name, &tmp) != 0)
			engine_named_set(factory_named[i].name, &factory_named[i].p);
	}
}

int engine_named_delete(const char *name) {
	if (!name || !*name) return 0;
	pthread_mutex_lock(&named_mtx);
	int i = named_find_locked(name);
	if (i < 0) { pthread_mutex_unlock(&named_mtx); return 0; }
	for (int j = i; j < named_count_v - 1; j++)
		named_presets[j] = named_presets[j + 1];
	named_count_v--;
	if (strcmp(named_active, name) == 0) named_active[0] = '\0';
	pthread_mutex_unlock(&named_mtx);
	return 1;
}

int engine_named_apply(const char *name) {
	engine_preset_t p;
	if (engine_named_get(name, &p) != 0) return -1;  /* backfill done inside */
	preset_apply_live(&p);
	atomic_store(&active_preset_slot, -1);            /* clear fixed slot */
	pthread_mutex_lock(&named_mtx);
	snprintf(named_active, ENGINE_NAME_LEN, "%s", name);
	pthread_mutex_unlock(&named_mtx);
	return 0;
}

const char *engine_named_active(void) {
	static char buf[ENGINE_NAME_LEN];
	pthread_mutex_lock(&named_mtx);
	snprintf(buf, sizeof(buf), "%s", named_active);
	pthread_mutex_unlock(&named_mtx);
	return buf;
}

void engine_named_set_active(const char *name) {
	pthread_mutex_lock(&named_mtx);
	if (name && *name) snprintf(named_active, ENGINE_NAME_LEN, "%s", name);
	else               named_active[0] = '\0';
	pthread_mutex_unlock(&named_mtx);
}
