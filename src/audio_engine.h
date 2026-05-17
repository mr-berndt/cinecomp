/*
 * audio_engine.h — public interface to the cinecomp DSP/JACK engine for
 * the standalone GUI build.
 *
 * The engine is the same DSP body as the aroio6-Buildroot package
 * `aroio_filmcomp` (Atomic params, audio_callback, JACK port wiring,
 * peak broadcast, OSC server). The standalone build wraps the engine
 * into a small public API so the ImGui front-end can:
 *   - start/stop the engine
 *   - read/write parameter values (Atomic-safe from any thread)
 *   - read live meter values for display
 *
 * OSC server inside the engine runs unchanged, so the standalone binary
 * keeps OSC remote-control parity with the Buildroot variant (same
 * /cinecomp/* paths, including the v2 zonal architecture knobs).
 */

#ifndef CINECOMP_AUDIO_ENGINE_H
#define CINECOMP_AUDIO_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_N_CHANNELS 8

/* Parameter IDs — one per knob/toggle the GUI exposes. Internally
 * mapped onto the engine's _Atomic globals. The float-IDs come first
 * (so they index a contiguous range), then the int/bool-IDs. */
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

    /* Zonal-architecture float params (v1 summed + v2 band-shaped). */
    PARAM_ATMO_THRESHOLD,
    PARAM_ATMO_MAX_GAIN,
    PARAM_ATMO_KNEE,
    PARAM_DIALOG_THRESHOLD,
    PARAM_DIALOG_MAX_GAIN,
    PARAM_DIALOG_KNEE,
    PARAM_NOISE_FLOOR_DB,
    PARAM_NOISE_KNEE_DB,
    PARAM_UPWARD_ATTACK_MS,
    PARAM_UPWARD_RELEASE_MS,
    PARAM_DUCK_ATTACK_MS,
    PARAM_DUCK_RELEASE_MS,

    PARAM__FLOAT_COUNT,

    PARAM_DETECTOR_MODE = PARAM__FLOAT_COUNT,   /* int 0=RMS, 1=Peak, 2=Dual */
    PARAM_DOWNWARD_EN,                          /* int 0/1 */
    PARAM_BYPASS,                               /* int 0/1 */
    PARAM_ARCHITECTURE_MODE,                    /* int 0=classic, 1=v1, 2=v2 */
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

/* Engine lifecycle. jack_name is the JACK client name (e.g. "cinecomp"),
 * osc_port the UDP port for the OSC server. Both can be 0/NULL to use
 * defaults ("cinecomp", 14041). Returns 0 on success. */
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
    int   architecture_mode;
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

/* ----------------------- Named presets --------------------------------- */
/* User-defined preset library — arbitrary names, no fixed slots. Mirrors
 * the aroio6 web UI: one dropdown with Factory (LOW/MID/HIGH) + Eigene
 * (named). Names are kept sorted for stable dropdown order. */

#define ENGINE_NAMED_MAX 64
#define ENGINE_NAME_LEN  33   /* 32 chars + NUL */

int  engine_named_count(void);
const char *engine_named_name(int idx);            /* idx 0..count-1, sorted */
int  engine_named_get(const char *name, engine_preset_t *out); /* 0=ok, -1=miss */
int  engine_named_save(const char *name);          /* snapshot live params; create/overwrite. 0=ok */
int  engine_named_delete(const char *name);        /* 1 if removed, 0 if absent */
int  engine_named_apply(const char *name);         /* load into live params; 0=ok, -1=miss */
const char *engine_named_active(void);             /* "" if none */
void engine_named_set_active(const char *name);    /* NULL/"" clears */

/* Persistence helper: create/overwrite a named preset directly from a
 * buffer (no live-param snapshot). Used by the state.ini loader. */
void engine_named_set(const char *name, const engine_preset_t *in);

/* Seed the built-in stock presets (90ies/Modern x Low/Mid/Hi). Only
 * adds names not already present, so it never clobbers a user copy.
 * Call before loading state so the user's state.ini wins. */
void engine_named_seed_factory(void);

#ifdef __cplusplus
}
#endif

#endif /* CINECOMP_AUDIO_ENGINE_H */
