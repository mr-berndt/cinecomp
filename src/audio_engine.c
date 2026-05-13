/*
 * aroio_filmcomp — Upward / parallel compressor for film playback.
 *
 * Topology: 8 inputs (7.1: L R C LFE LS RS RBL RBR) → side-chain
 * detector + gain computer → 8 outputs (1:1 channels, linked gain).
 *
 * Realtime-safe audio thread (no malloc, no locks). OSC server and
 * peak reporter run in separate threads; communication via _Atomic.
 *
 * Sits between input_mix-volctl and BruteFIR in the aroio chain, so
 * the comp sees raw program dynamics (not EQ-colored signal). Plugin
 * has fixed 8 in/8 out ports — works correctly for stereo / 5.1 / 7.1
 * via smooth per-channel weights (silent channels drop out of the SC
 * detection automatically, no layout declaration needed).
 *
 * OSC API (UDP, default port 14041):
 *   in:
 *     /filmcomp/threshold f          — dBFS for SC
 *     /filmcomp/ratio f              — 1.0 .. 20.0 (upward)
 *     /filmcomp/attack_ms f          — how fast boost ramps up
 *     /filmcomp/release_ms f         — how fast boost decays
 *     /filmcomp/hold_ms f            — anti-pump hold between A and R
 *     /filmcomp/knee_db f            — soft-knee width
 *     /filmcomp/makeup_db f          — post-comp static gain
 *     /filmcomp/wet_dry f            — 0.0=dry .. 1.0=full comp
 *     /filmcomp/rms_win_ms f         — detector smoothing time
 *     /filmcomp/sc_hpf_hz f          — side-chain HPF corner
 *     /filmcomp/lookahead_ms f       — audio path delay (anti-overshoot)
 *     /filmcomp/max_gain f           — hard cap for upward boost (dB)
 *     /filmcomp/detector i           — 0=RMS broadband, 1=Peak follower
 *     /filmcomp/downward/enable i    — 0/1
 *     /filmcomp/downward/threshold f
 *     /filmcomp/downward/ratio f
 *     /filmcomp/bypass i             — 0/1 (default 1 at boot)
 *     /filmcomp/preset/select i      — apply preset N (0=low 1=mid 2=high)
 *     /filmcomp/preset/save   i      — save current params to preset N
 *     /filmcomp/preset/reset  i      — reset preset N to factory defaults
 *     /filmcomp/subscribe            — sender added to broadcast list
 *     /filmcomp/unsubscribe          — sender removed
 *     /filmcomp/get                  — request current state
 *   out (broadcast to subscribers, ~20 Hz):
 *     /filmcomp/peaks_in ff…f        — 8 peak dBFS per input channel
 *     /filmcomp/peaks_out ff…f       — 8 peak dBFS per output channel
 *     /filmcomp/weights ff…f         — 8 channel weights 0..1 (activity)
 *     /filmcomp/sc_db f              — current side-chain RMS dB
 *     /filmcomp/gain_db f            — current applied gain (before makeup)
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

#include <jack/jack.h>

#include "audio_engine.h"

/* FTZ/DAZ: flush sub-normal floats. Same rationale as in aroio_volctl —
 * smoothing through denormals on x86 causes severe slowdowns. */
#if defined(__x86_64__) || defined(__i386__)
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#  define AROIO_RT_INIT_FPU() do { \
       _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON); \
       _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON); \
   } while (0)
#elif defined(__aarch64__)
#  define AROIO_RT_INIT_FPU() do { \
       uint64_t fpcr; \
       __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr)); \
       fpcr |= (1ULL << 24); \
       __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr)); \
   } while (0)
#else
#  define AROIO_RT_INIT_FPU() do { } while (0)
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
static const char *jack_name = "filmcomp";
static int verbose = 0;

static jack_client_t *client;
static jack_port_t *in_ports[N_CHANNELS];
static jack_port_t *out_ports[N_CHANNELS];
static jack_nframes_t sample_rate = 48000;

/* Parameters — set by OSC, read by audio thread.
 *
 * Defaults below match the "Mid" preset tuned 2026-05-12. Two test
 * scenes drove the values: a two-gunshot scene (transient leak) and
 * the alarm-clock-ticks-over-atmo from "A Quiet Place" (~1 Hz tick
 * cycle was modulating the background ambience). Properties:
 *   - threshold -10.5 lets loud RMS content pass without lift; only
 *     dialog/ambient (SC < -10.5) gets boost
 *   - ratio 3 + max_gain 10 give a clean +10 dB on quiet stuff
 *   - release 10 + lookahead 15: transients reach the multiplier
 *     after ~1.5τ of release → ≤ +2 dB leak on shots
 *   - hold 0: peak detector holds itself; an extra hold gate would
 *     just block release on transients
 *   - rms_win 500 (peak release tau): slow enough that the gain
 *     doesn't fully recover between repeated transients (alarm-clock
 *     ticks etc.) → no audible boost modulation on the background
 *   - makeup -0.5: small trim, kept near 0 so loud material stays
 *     comparable to bypass */
static _Atomic float p_threshold_db   = -10.5f;
static _Atomic float p_ratio          =   3.0f;
static _Atomic float p_attack_ms      =   5.0f;
static _Atomic float p_release_ms     =  10.0f;
static _Atomic float p_hold_ms        =   0.0f;
static _Atomic float p_knee_db        =   6.0f;
static _Atomic float p_makeup_db      =  -0.5f;
static _Atomic float p_wet_dry        =   1.0f;
static _Atomic float p_rms_win_ms     = 500.0f;
static _Atomic float p_sc_hpf_hz      =  60.0f;
static _Atomic float p_lookahead_ms   =  15.0f;
static _Atomic float p_max_gain_db    =  10.0f;
/* 0 = broadband RMS (smoother on y² with rms_win_ms time constant).
 * 1 = peak follower (instant attack, exponential release; rms_win_ms
 *     acts as the release tau). Peak mode reacts to transients in
 *     <1 sample so look-ahead actually works as designed, and the
 *     decay is fast enough that a second transient ~50 ms later gets
 *     symmetric treatment instead of being clamped by the first one's
 *     RMS tail. Matches the detector type used by ffmpeg/mpv.
 *
 * Default 1 (Peak) — verified on film/gunshot material 2026-05-12. */
static _Atomic int   p_detector_mode  =   1;
static _Atomic int   p_downward_en    =   0;
static _Atomic float p_down_threshold =  -6.0f;
static _Atomic float p_down_ratio     =   4.0f;
static _Atomic int   p_bypass         =   1;   /* default ON: pass-through */

/* Audio-thread-local state (only audio thread writes & reads).
 * Initialised once in audio_callback first run. */
static float rms_state[N_CHANNELS];
static float peak_state[N_CHANNELS];   /* peak-follower output, mode=1 only */
static float sc_hpf_z1[N_CHANNELS], sc_hpf_z2[N_CHANNELS];
static float hpf_b0, hpf_b1, hpf_b2, hpf_a1, hpf_a2;
static float current_hpf_hz = 0.0f;
static float gain_current_db = 0.0f;
static int   hold_counter = 0;
static float delay_buf[N_CHANNELS][MAX_LOOKAHEAD_SAMPLES];
static int   delay_pos = 0;
static int   delay_initialised = 0;

/* Meters — written by audio thread (atomic), read by peak reporter. */
static _Atomic float peak_in_acc[N_CHANNELS];
static _Atomic float peak_out_acc[N_CHANNELS];
static _Atomic float weight_acc[N_CHANNELS];
static _Atomic float current_sc_db    = -INFINITY;
static _Atomic float current_gain_atomic_db = 0.0f;

static int osc_socket = -1;
static struct sockaddr_in subscribers[MAX_SUBSCRIBERS];
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

static int audio_callback(jack_nframes_t nframes, void *arg) {
	(void)arg;
	static int rt_init_done = 0;
	if (!rt_init_done) {
		AROIO_RT_INIT_FPU();
		rt_init_done = 1;
	}

	/* One-time init of large RT state. */
	if (!delay_initialised) {
		for (int ch = 0; ch < N_CHANNELS; ch++) {
			for (int i = 0; i < MAX_LOOKAHEAD_SAMPLES; i++)
				delay_buf[ch][i] = 0.0f;
			rms_state[ch] = 1e-18f;
			peak_state[ch] = 0.0f;
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
	const float makeup_db     = atomic_load_explicit(&p_makeup_db,    memory_order_relaxed);
	const float wet_dry       = fclampf(atomic_load_explicit(&p_wet_dry, memory_order_relaxed), 0.0f, 1.0f);
	const float rms_win_ms    = atomic_load_explicit(&p_rms_win_ms,   memory_order_relaxed);
	const float sc_hpf_hz     = atomic_load_explicit(&p_sc_hpf_hz,    memory_order_relaxed);
	const float lookahead_ms  = atomic_load_explicit(&p_lookahead_ms, memory_order_relaxed);
	const float max_gain_db   = atomic_load_explicit(&p_max_gain_db,  memory_order_relaxed);
	const int   detector_mode = atomic_load_explicit(&p_detector_mode, memory_order_relaxed);
	const int   downward_en   = atomic_load_explicit(&p_downward_en,  memory_order_relaxed);
	const float down_thr      = atomic_load_explicit(&p_down_threshold, memory_order_relaxed);
	const float down_ratio    = atomic_load_explicit(&p_down_ratio,   memory_order_relaxed);
	const int   bypass        = atomic_load_explicit(&p_bypass,       memory_order_relaxed);

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

	float *in[N_CHANNELS], *out[N_CHANNELS];
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		in[ch]  = jack_port_get_buffer(in_ports[ch], nframes);
		out[ch] = jack_port_get_buffer(out_ports[ch], nframes);
	}

	/* Per-block peak accumulators (local, then published atomic at block end). */
	float peak_in_block[N_CHANNELS]  = {0};
	float peak_out_block[N_CHANNELS] = {0};
	float weight_block[N_CHANNELS]   = {0};
	float sc_block_last = -120.0f;
	float gain_block_last = 0.0f;

	for (jack_nframes_t i = 0; i < nframes; i++) {
		/* ---- Per-channel detection ---- */
		float sum_w = 0.0f, sum_we = 0.0f;

		for (int ch = 0; ch < N_CHANNELS; ch++) {
			float x = in[ch][i];
			float ax = fabsf(x);
			if (ax > peak_in_block[ch]) peak_in_block[ch] = ax;

			/* SC-HPF on the per-channel signal. */
			float y = biquad_step(x, &sc_hpf_z1[ch], &sc_hpf_z2[ch],
			                       hpf_b0, hpf_b1, hpf_b2,
			                       hpf_a1, hpf_a2);

			/* Detector: either RMS smoother on y², or peak follower
			 * with instant attack + exponential release. Peak is
			 * far better at catching transients (gunshots, doors)
			 * because the level captures the rising edge in <1 sample
			 * — the RMS smoother's tau-of-rms_win_ms would lag the
			 * transient and let it through boosted. Layout-agnostic
			 * either way: the per-channel weight handles silent ports. */
			float level;
			if (detector_mode == 1) {
				float ay = fabsf(y);
				if (ay > peak_state[ch]) peak_state[ch] = ay;
				else                     peak_state[ch] *= peak_decay;
				level = peak_state[ch];
			} else {
				rms_state[ch] += (y * y - rms_state[ch]) * rms_alpha;
				level = sqrtf(rms_state[ch] + 1e-20f);
			}

			/* Smooth activity weight: 0 at ≤ -85 dB, 1 at ≥ -75 dB. */
			float level_db = lin_to_db(level);
			float w = smoothstep_f(
				ACTIVITY_FLOOR_DB - ACTIVITY_SLOPE_DB,
				ACTIVITY_FLOOR_DB + ACTIVITY_SLOPE_DB,
				level_db);
			if (w > weight_block[ch]) weight_block[ch] = w;

			sum_w  += w;
			sum_we += w * level * level;
		}

		/* SC = energy-mean of active channels.
		 * Falls back to silence if no active channel. */
		float sc;
		if (sum_w > 1e-6f) {
			sc = sqrtf(sum_we / sum_w);
		} else {
			sc = 0.0f;
		}
		float sc_db = lin_to_db(sc);
		sc_block_last = sc_db;

		/* ---- Gain target ---- */
		float gain_up   = upward_gain_db(sc_db, threshold, ratio, knee_db);
		/* Hard cap on upward boost: when very low-level program (or
		 * leftover noise floor) drives the SC far below threshold,
		 * (T - sc) * (1 - 1/R) grows without bound and produces
		 * audible noise pumping. Clamp before adding makeup. */
		if (gain_up > max_gain_db) gain_up = max_gain_db;
		float gain_down = downward_en
		                  ? downward_gain_db(sc_db, down_thr, down_ratio, knee_db)
		                  : 0.0f;
		float gain_target_db = gain_up + gain_down;

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
		/* Note: hold_counter is loaded each time attack is sustained
		 * — i.e. when boost is steady-high, we keep refreshing the
		 * hold so a brief peak doesn't trigger release. Done above
		 * implicitly: every time gain_target == gain_current after
		 * an attack settle, hold resets. */

		gain_block_last = gain_current_db;

		float gain_lin = db_to_lin(gain_current_db + makeup_db);

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
				float wet = in_delayed * gain_lin;
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
		float prev_in = atomic_load_explicit(&peak_in_acc[ch], memory_order_relaxed);
		if (peak_in_block[ch] > prev_in) {
			atomic_store_explicit(&peak_in_acc[ch], peak_in_block[ch], memory_order_relaxed);
		}
		float prev_out = atomic_load_explicit(&peak_out_acc[ch], memory_order_relaxed);
		if (peak_out_block[ch] > prev_out) {
			atomic_store_explicit(&peak_out_acc[ch], peak_out_block[ch], memory_order_relaxed);
		}
		atomic_store_explicit(&weight_acc[ch], weight_block[ch], memory_order_relaxed);
	}
	atomic_store_explicit(&current_sc_db,           sc_block_last,   memory_order_relaxed);
	atomic_store_explicit(&current_gain_atomic_db,  gain_block_last, memory_order_relaxed);

	return 0;
}

static int sample_rate_callback(jack_nframes_t nframes, void *arg) {
	(void)arg;
	sample_rate = nframes;
	/* Force HPF recompute next audio block. */
	current_hpf_hz = 0.0f;
	return 0;
}

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

/* Snapshot current state, send to subscribers. Called by peak reporter. */
static void broadcast_meters(void) {
	float pin[N_CHANNELS], pout[N_CHANNELS], wts[N_CHANNELS];
	for (int ch = 0; ch < N_CHANNELS; ch++) {
		float p_in_lin  = atomic_exchange(&peak_in_acc[ch],  0.0f);
		float p_out_lin = atomic_exchange(&peak_out_acc[ch], 0.0f);
		pin[ch]  = lin_to_db(p_in_lin);
		pout[ch] = lin_to_db(p_out_lin);
		wts[ch]  = atomic_load_explicit(&weight_acc[ch], memory_order_relaxed);
	}
	float sc_db   = atomic_load_explicit(&current_sc_db,          memory_order_relaxed);
	float gain_db = atomic_load_explicit(&current_gain_atomic_db, memory_order_relaxed);

	pthread_mutex_lock(&subscriber_mtx);
	for (int s = 0; s < subscriber_count; s++) {
		send_eight_floats(&subscribers[s], "/filmcomp/peaks_in",  pin);
		send_eight_floats(&subscribers[s], "/filmcomp/peaks_out", pout);
		send_eight_floats(&subscribers[s], "/filmcomp/weights",   wts);
		send_one_float(&subscribers[s], "/filmcomp/sc_db",   sc_db);
		send_one_float(&subscribers[s], "/filmcomp/gain_db", gain_db);
	}
	pthread_mutex_unlock(&subscriber_mtx);
}

static void send_state(struct sockaddr_in *dst) {
	send_one_float(dst, "/filmcomp/threshold",  atomic_load_explicit(&p_threshold_db, memory_order_relaxed));
	send_one_float(dst, "/filmcomp/ratio",      atomic_load_explicit(&p_ratio,        memory_order_relaxed));
	send_one_float(dst, "/filmcomp/attack_ms",  atomic_load_explicit(&p_attack_ms,    memory_order_relaxed));
	send_one_float(dst, "/filmcomp/release_ms", atomic_load_explicit(&p_release_ms,   memory_order_relaxed));
	send_one_float(dst, "/filmcomp/hold_ms",    atomic_load_explicit(&p_hold_ms,      memory_order_relaxed));
	send_one_float(dst, "/filmcomp/knee_db",    atomic_load_explicit(&p_knee_db,      memory_order_relaxed));
	send_one_float(dst, "/filmcomp/makeup_db",  atomic_load_explicit(&p_makeup_db,    memory_order_relaxed));
	send_one_float(dst, "/filmcomp/wet_dry",    atomic_load_explicit(&p_wet_dry,      memory_order_relaxed));
	send_one_float(dst, "/filmcomp/rms_win_ms", atomic_load_explicit(&p_rms_win_ms,   memory_order_relaxed));
	send_one_float(dst, "/filmcomp/sc_hpf_hz",  atomic_load_explicit(&p_sc_hpf_hz,    memory_order_relaxed));
	send_one_float(dst, "/filmcomp/lookahead_ms", atomic_load_explicit(&p_lookahead_ms, memory_order_relaxed));
	send_one_float(dst, "/filmcomp/max_gain",   atomic_load_explicit(&p_max_gain_db,  memory_order_relaxed));
	send_one_int  (dst, "/filmcomp/detector",   atomic_load_explicit(&p_detector_mode, memory_order_relaxed));
	send_one_int  (dst, "/filmcomp/downward/enable",   atomic_load_explicit(&p_downward_en,    memory_order_relaxed));
	send_one_float(dst, "/filmcomp/downward/threshold", atomic_load_explicit(&p_down_threshold, memory_order_relaxed));
	send_one_float(dst, "/filmcomp/downward/ratio",     atomic_load_explicit(&p_down_ratio,     memory_order_relaxed));
	send_one_int  (dst, "/filmcomp/bypass",     atomic_load_explicit(&p_bypass,       memory_order_relaxed));
	send_one_int  (dst, "/filmcomp/preset/active", engine_preset_active());
}

static int sockaddr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b) {
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void subscribe(const struct sockaddr_in *src) {
	pthread_mutex_lock(&subscriber_mtx);
	for (int i = 0; i < subscriber_count; i++) {
		if (sockaddr_eq(&subscribers[i], src)) {
			pthread_mutex_unlock(&subscriber_mtx);
			return;
		}
	}
	if (subscriber_count < MAX_SUBSCRIBERS) {
		subscribers[subscriber_count++] = *src;
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
			subscribers[i] = subscribers[--subscriber_count];
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

	if (strcmp(path, "/filmcomp/threshold")  == 0) atomic_store(&p_threshold_db, fclampf(READ_F(0.0f), -60.0f, 0.0f));
	else if (strcmp(path, "/filmcomp/ratio") == 0) atomic_store(&p_ratio,        fclampf(READ_F(3.5f), 1.0f, 20.0f));
	else if (strcmp(path, "/filmcomp/attack_ms")  == 0) atomic_store(&p_attack_ms,  fclampf(READ_F(30.0f), 1.0f, 200.0f));
	else if (strcmp(path, "/filmcomp/release_ms") == 0) atomic_store(&p_release_ms, fclampf(READ_F(400.0f), 10.0f, 2000.0f));
	else if (strcmp(path, "/filmcomp/hold_ms") == 0) atomic_store(&p_hold_ms, fclampf(READ_F(25.0f), 0.0f, 200.0f));
	else if (strcmp(path, "/filmcomp/knee_db") == 0) atomic_store(&p_knee_db, fclampf(READ_F(6.0f), 0.0f, 20.0f));
	else if (strcmp(path, "/filmcomp/makeup_db") == 0) atomic_store(&p_makeup_db, fclampf(READ_F(0.0f), -12.0f, 18.0f));
	else if (strcmp(path, "/filmcomp/wet_dry") == 0) atomic_store(&p_wet_dry, fclampf(READ_F(1.0f), 0.0f, 1.0f));
	else if (strcmp(path, "/filmcomp/rms_win_ms") == 0) atomic_store(&p_rms_win_ms, fclampf(READ_F(300.0f), 10.0f, 1000.0f));
	else if (strcmp(path, "/filmcomp/sc_hpf_hz") == 0) atomic_store(&p_sc_hpf_hz, fclampf(READ_F(60.0f), 20.0f, 500.0f));
	else if (strcmp(path, "/filmcomp/lookahead_ms") == 0) atomic_store(&p_lookahead_ms, fclampf(READ_F(5.0f), 0.0f, 20.0f));
	else if (strcmp(path, "/filmcomp/max_gain") == 0) atomic_store(&p_max_gain_db, fclampf(READ_F(12.0f), 0.0f, 30.0f));
	else if (strcmp(path, "/filmcomp/detector") == 0) atomic_store(&p_detector_mode, READ_I(0) ? 1 : 0);
	else if (strcmp(path, "/filmcomp/downward/enable") == 0) atomic_store(&p_downward_en, READ_I(0) ? 1 : 0);
	else if (strcmp(path, "/filmcomp/downward/threshold") == 0) atomic_store(&p_down_threshold, fclampf(READ_F(-6.0f), -60.0f, 0.0f));
	else if (strcmp(path, "/filmcomp/downward/ratio") == 0) atomic_store(&p_down_ratio, fclampf(READ_F(4.0f), 1.0f, 20.0f));
	else if (strcmp(path, "/filmcomp/bypass") == 0) atomic_store(&p_bypass, READ_I(1) ? 1 : 0);
	else if (strcmp(path, "/filmcomp/preset/select") == 0) engine_preset_apply((engine_preset_slot_t)READ_I(1));
	else if (strcmp(path, "/filmcomp/preset/save")   == 0) engine_preset_save((engine_preset_slot_t)READ_I(engine_preset_active() < 0 ? 1 : engine_preset_active()));
	else if (strcmp(path, "/filmcomp/preset/reset")  == 0) engine_preset_reset((engine_preset_slot_t)READ_I(engine_preset_active() < 0 ? 1 : engine_preset_active()));
	else if (strcmp(path, "/filmcomp/subscribe") == 0) subscribe(src);
	else if (strcmp(path, "/filmcomp/unsubscribe") == 0) unsubscribe(src);
	else if (strcmp(path, "/filmcomp/get") == 0) send_state(src);
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

	update_hpf_coeffs(60.0f, (float)sample_rate);
	current_hpf_hz = 60.0f;

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

unsigned engine_sample_rate(void) {
	return sample_rate;
}

/* Param-table dispatch: each enum maps to one atomic. Done with two
 * switches (one per type) to keep this branch-predictor-friendly. */
float engine_get_param_f(engine_param_t id) {
	switch (id) {
	case PARAM_THRESHOLD:      return atomic_load(&p_threshold_db);
	case PARAM_RATIO:          return atomic_load(&p_ratio);
	case PARAM_ATTACK_MS:      return atomic_load(&p_attack_ms);
	case PARAM_RELEASE_MS:     return atomic_load(&p_release_ms);
	case PARAM_HOLD_MS:        return atomic_load(&p_hold_ms);
	case PARAM_KNEE_DB:        return atomic_load(&p_knee_db);
	case PARAM_MAKEUP_DB:      return atomic_load(&p_makeup_db);
	case PARAM_WET_DRY:        return atomic_load(&p_wet_dry);
	case PARAM_RMS_WIN_MS:     return atomic_load(&p_rms_win_ms);
	case PARAM_SC_HPF_HZ:      return atomic_load(&p_sc_hpf_hz);
	case PARAM_LOOKAHEAD_MS:   return atomic_load(&p_lookahead_ms);
	case PARAM_MAX_GAIN_DB:    return atomic_load(&p_max_gain_db);
	case PARAM_DOWN_THRESHOLD: return atomic_load(&p_down_threshold);
	case PARAM_DOWN_RATIO:     return atomic_load(&p_down_ratio);
	default: return 0.0f;
	}
}

void engine_set_param_f(engine_param_t id, float v) {
	switch (id) {
	case PARAM_THRESHOLD:      atomic_store(&p_threshold_db, fclampf(v, -60.0f, 0.0f)); break;
	case PARAM_RATIO:          atomic_store(&p_ratio,        fclampf(v, 1.0f, 20.0f)); break;
	case PARAM_ATTACK_MS:      atomic_store(&p_attack_ms,    fclampf(v, 1.0f, 200.0f)); break;
	case PARAM_RELEASE_MS:     atomic_store(&p_release_ms,   fclampf(v, 10.0f, 2000.0f)); break;
	case PARAM_HOLD_MS:        atomic_store(&p_hold_ms,      fclampf(v, 0.0f, 200.0f)); break;
	case PARAM_KNEE_DB:        atomic_store(&p_knee_db,      fclampf(v, 0.0f, 20.0f)); break;
	case PARAM_MAKEUP_DB:      atomic_store(&p_makeup_db,    fclampf(v, -12.0f, 18.0f)); break;
	case PARAM_WET_DRY:        atomic_store(&p_wet_dry,      fclampf(v, 0.0f, 1.0f)); break;
	case PARAM_RMS_WIN_MS:     atomic_store(&p_rms_win_ms,   fclampf(v, 10.0f, 2000.0f)); break;
	case PARAM_SC_HPF_HZ:      atomic_store(&p_sc_hpf_hz,    fclampf(v, 20.0f, 500.0f)); break;
	case PARAM_LOOKAHEAD_MS:   atomic_store(&p_lookahead_ms, fclampf(v, 0.0f, 20.0f)); break;
	case PARAM_MAX_GAIN_DB:    atomic_store(&p_max_gain_db,  fclampf(v, 0.0f, 30.0f)); break;
	case PARAM_DOWN_THRESHOLD: atomic_store(&p_down_threshold, fclampf(v, -60.0f, 0.0f)); break;
	case PARAM_DOWN_RATIO:     atomic_store(&p_down_ratio,   fclampf(v, 1.0f, 20.0f)); break;
	default: break;
	}
}

int engine_get_param_i(engine_param_t id) {
	switch (id) {
	case PARAM_DETECTOR_MODE: return atomic_load(&p_detector_mode);
	case PARAM_DOWNWARD_EN:   return atomic_load(&p_downward_en);
	case PARAM_BYPASS:        return atomic_load(&p_bypass);
	default: return 0;
	}
}

void engine_set_param_i(engine_param_t id, int v) {
	switch (id) {
	case PARAM_DETECTOR_MODE: atomic_store(&p_detector_mode, v ? 1 : 0); break;
	case PARAM_DOWNWARD_EN:   atomic_store(&p_downward_en,   v ? 1 : 0); break;
	case PARAM_BYPASS:        atomic_store(&p_bypass,        v ? 1 : 0); break;
	default: break;
	}
}

void engine_read_meters(engine_meters_t *out) {
	if (!out) return;
	for (int ch = 0; ch < ENGINE_N_CHANNELS; ch++) {
		float p_in  = atomic_exchange(&peak_in_acc[ch],  0.0f);
		float p_out = atomic_exchange(&peak_out_acc[ch], 0.0f);
		out->peak_in[ch]  = lin_to_db(p_in);
		out->peak_out[ch] = lin_to_db(p_out);
		out->weight[ch]   = atomic_load_explicit(&weight_acc[ch], memory_order_relaxed);
	}
	out->sc_db   = atomic_load_explicit(&current_sc_db,           memory_order_relaxed);
	out->gain_db = atomic_load_explicit(&current_gain_atomic_db,  memory_order_relaxed);
}

/* ----------------------- Presets ---------------------------------------
 * Three named slots. Factory defaults mirror Nicola's mpv compressor
 * profiles (LOW/MID/HIGH) but populate all 14 filmcomp float params plus
 * detector_mode + downward_en. MID matches the boot defaults above
 * (Wecker-getestet 2026-05-12). LOW is gentle (low ratio, sparingly
 * boost); HIGH is aggressive (more lift, lower threshold). */
/* Factory-presets aligned to Nicola's tuning (2026-05-13, cinema-
 * verified on convolver against John-Wick double-shot, A-Quiet-Place
 * tinker scene, Wecker scene). All three share fast peak detection
 * (sc_hpf=20 catches LFE thumps in SC), 20ms lookahead, knee 6.1.
 *
 *   LOW  — konservativer Loudness-Lift mit Knee-Rolloff (Duck off).
 *          +5.7 dB Atmo, dialog +4, Action sanft gerundet.
 *   MID  — echte Dynamik-Range-Compression (Duck on -10/5:1).
 *          +10 dB Atmo, dialog +7.5, Action -5.6.
 *   HIGH — Dialog-Max (Lift bis Cap-Dialog) für arge Modern-Cinema-
 *          Dynamik. +14 dB Atmo+Dialog, Action -5.6. */
static const engine_preset_t factory_presets[PRESET__COUNT] = {
	[PRESET_LOW] = {
		.f = {
			[PARAM_THRESHOLD]    =   0.0f,
			[PARAM_RATIO]        =  3.6f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 56.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      =  6.1f,
			[PARAM_MAKEUP_DB]    = -10.3f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 33.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 16.3f,
			[PARAM_DOWN_THRESHOLD] = -6.0f,
			[PARAM_DOWN_RATIO]   =  2.5f,
		},
		.detector_mode = 1,
		.downward_en   = 0,
	},
	[PRESET_MID] = {
		.f = {
			[PARAM_THRESHOLD]    = -10.0f,
			[PARAM_RATIO]        =  4.0f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 20.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      =  6.1f,
			[PARAM_MAKEUP_DB]    =  0.0f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 60.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 10.0f,
			[PARAM_DOWN_THRESHOLD] = -6.0f,
			[PARAM_DOWN_RATIO]   =  4.0f,
		},
		.detector_mode = 1,
		.downward_en   = 1,
	},
	[PRESET_HIGH] = {
		.f = {
			[PARAM_THRESHOLD]    = -10.0f,
			[PARAM_RATIO]        =  6.0f,
			[PARAM_ATTACK_MS]    =  5.0f,
			[PARAM_RELEASE_MS]   = 20.0f,
			[PARAM_HOLD_MS]      =  0.0f,
			[PARAM_KNEE_DB]      =  6.1f,
			[PARAM_MAKEUP_DB]    =  0.0f,
			[PARAM_WET_DRY]      =  1.0f,
			[PARAM_RMS_WIN_MS]   = 60.0f,
			[PARAM_SC_HPF_HZ]    = 20.0f,
			[PARAM_LOOKAHEAD_MS] = 20.0f,
			[PARAM_MAX_GAIN_DB]  = 14.0f,
			[PARAM_DOWN_THRESHOLD] = -6.0f,
			[PARAM_DOWN_RATIO]   =  4.0f,
		},
		.detector_mode = 1,
		.downward_en   = 1,
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

int engine_preset_apply(engine_preset_slot_t slot) {
	if (slot < 0 || slot >= PRESET__COUNT) return -1;
	preset_init_once();
	pthread_mutex_lock(&preset_mtx);
	engine_preset_t p = presets[slot];
	pthread_mutex_unlock(&preset_mtx);

	for (int i = 0; i < PARAM__FLOAT_COUNT; i++) {
		engine_set_param_f((engine_param_t)i, p.f[i]);
	}
	engine_set_param_i(PARAM_DETECTOR_MODE, p.detector_mode);
	engine_set_param_i(PARAM_DOWNWARD_EN,   p.downward_en);
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
	p.detector_mode = engine_get_param_i(PARAM_DETECTOR_MODE);
	p.downward_en   = engine_get_param_i(PARAM_DOWNWARD_EN);

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
