/*
 * audio_engine.h — public interface to the aroio_filmcomp DSP/JACK
 * engine for the standalone GUI build.
 *
 * The engine is what was the entire body of aroio_filmcomp.c (Atomic
 * params, audio_callback, JACK port wiring, peak broadcast); we lift
 * out only what the GUI thread needs:
 *   - start/stop the engine
 *   - read/write parameter values (Atomic-safe from any thread)
 *   - read live meter values for display
 *
 * The OSC server inside the engine continues to run unchanged, so the
 * standalone binary keeps OSC remote-control parity with the Buildroot
 * version (same OSC paths under the /filmcomp/ tree).
 */

#ifndef AROIO_FILMCOMP_AUDIO_ENGINE_H
#define AROIO_FILMCOMP_AUDIO_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_N_CHANNELS 8

/* Parameter IDs — one per knob/toggle the GUI exposes. Internally
 * mapped onto the engine's _Atomic globals. */
typedef enum {
    PARAM_THRESHOLD,
    PARAM_RATIO,
    PARAM_ATTACK_MS,
    PARAM_RELEASE_MS,
    PARAM_HOLD_MS,
    PARAM_KNEE_DB,
    PARAM_MAKEUP_DB,
    PARAM_WET_DRY,
    PARAM_RMS_WIN_MS,
    PARAM_SC_HPF_HZ,
    PARAM_LOOKAHEAD_MS,
    PARAM_MAX_GAIN_DB,
    PARAM_DOWN_THRESHOLD,
    PARAM_DOWN_RATIO,
    PARAM__FLOAT_COUNT,

    PARAM_DETECTOR_MODE = PARAM__FLOAT_COUNT,   /* int 0=RMS, 1=Peak */
    PARAM_DOWNWARD_EN,                          /* int 0/1 */
    PARAM_BYPASS,                               /* int 0/1 */
    PARAM__TOTAL_COUNT
} engine_param_t;

/* Snapshot of live meters. Filled by engine_read_meters(). */
typedef struct {
    float peak_in[ENGINE_N_CHANNELS];   /* dBFS, -120 = silent */
    float peak_out[ENGINE_N_CHANNELS];  /* dBFS */
    float weight[ENGINE_N_CHANNELS];    /* 0..1 channel activity */
    float sc_db;                        /* side-chain RMS in dB */
    float gain_db;                      /* currently applied upward gain */
} engine_meters_t;

/* Engine lifecycle. jack_name is the JACK client name (e.g. "filmcomp"),
 * osc_port the UDP port for the OSC server. Both can be 0/NULL to use
 * defaults ("filmcomp", 14041). Returns 0 on success. */
int  engine_start(const char *jack_name, int osc_port);
void engine_stop(void);
unsigned engine_sample_rate(void);

/* Parameter accessors. Float param_id < PARAM__FLOAT_COUNT use the
 * _f variants; int params use the _i variants. */
float engine_get_param_f(engine_param_t id);
void  engine_set_param_f(engine_param_t id, float value);
int   engine_get_param_i(engine_param_t id);
void  engine_set_param_i(engine_param_t id, int value);

/* Take a non-destructive snapshot of meters. Safe to call from any
 * thread; cheap (atomic loads only). Peak-in/out values are CONSUMED
 * — calling resets them so the GUI sees per-frame maxima. */
void engine_read_meters(engine_meters_t *out);

/* ----------------------- Presets --------------------------------------- */

typedef enum {
    PRESET_LOW       = 0,
    PRESET_MID       = 1,
    PRESET_HIGH      = 2,
    PRESET__COUNT
} engine_preset_slot_t;

/* Snapshot of all preset-controlled params (everything except bypass).
 * Returned by engine_preset_get(); accepted by engine_preset_set(). The
 * field order is stable and mirrors PARAM_* IDs up to PARAM__FLOAT_COUNT
 * for floats; ints follow. */
typedef struct {
    float f[PARAM__FLOAT_COUNT];
    int   detector_mode;
    int   downward_en;
} engine_preset_t;

/* Apply a preset: copy preset[slot] into live atomic params, mark as
 * active. Returns 0 on success. */
int  engine_preset_apply(engine_preset_slot_t slot);

/* Save: read current live param values into preset[slot]. */
void engine_preset_save(engine_preset_slot_t slot);

/* Reset preset[slot] back to factory defaults. */
void engine_preset_reset(engine_preset_slot_t slot);

/* Active preset slot getter (last applied). May be -1 if nothing
 * applied yet (initial boot, custom edits). */
int  engine_preset_active(void);

/* GUI-facing get/set for persistence to disk. */
void engine_preset_get(engine_preset_slot_t slot, engine_preset_t *out);
void engine_preset_set(engine_preset_slot_t slot, const engine_preset_t *in);

/* Label string for a slot, e.g. "low", "mid", "high". */
const char *engine_preset_name(engine_preset_slot_t slot);

#ifdef __cplusplus
}
#endif

#endif /* AROIO_FILMCOMP_AUDIO_ENGINE_H */
