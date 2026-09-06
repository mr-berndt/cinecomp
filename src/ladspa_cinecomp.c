/* cinecomp as a LADSPA plugin.
 *
 * Same DSP as the JACK client - engine_process_block() is literally the code
 * that has always run in the JACK callback. This file only provides the other
 * kind of host: no server, no ports to connect, no patchbay. mpv loads it with
 *
 *     af=ladspa=file=cinecomp_ladspa:plugin=cinecomp_stereo:controls=c0=-18|c1=2
 *
 * Two variants are exported. The DSP is built for 8 channels with a linked
 * side-chain, and that linkage is the point - so the stereo variant does not
 * run two independent instances, it feeds the eight-channel engine and leaves
 * the surround inputs silent.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../vendor/ladspa.h"
#include "audio_engine.h"

#define ENG_CH   ENGINE_N_CHANNELS          /* 8: L R C LFE LS RS RBL RBR */
#define N_CTL    PARAM__TOTAL_COUNT

/* Silence handed to the engine for channels the host does not provide. Read
 * only, shared, and never written by the DSP: out-of-range outputs get their
 * own scratch buffer below. */
#define MAX_BLOCK 8192
static LADSPA_Data silence[MAX_BLOCK];
static LADSPA_Data scratch[ENG_CH][MAX_BLOCK];

typedef struct {
    unsigned          nch;                  /* channels the host connects */
    LADSPA_Data      *in[ENG_CH];
    LADSPA_Data      *out[ENG_CH];
    LADSPA_Data      *ctl[N_CTL];
    int               ctl_taken;
} cinecomp_t;

/* ------------------------------------------------------------------ ports */

static const char *const ctl_name[N_CTL] = {
    "Threshold (dB)",        "Ratio",                 "Attack (ms)",
    "Release (ms)",          "Hold (ms)",             "Knee (dB)",
    "Makeup (dB)",           "Wet/Dry",               "RMS window (ms)",
    "SC highpass (Hz)",      "Lookahead (ms)",        "Max gain (dB)",
    "Downward threshold",    "Downward ratio",
    "Atmo threshold (dB)",   "Atmo max gain (dB)",    "Atmo knee (dB)",
    "Dialog threshold (dB)", "Dialog max gain (dB)",  "Dialog knee (dB)",
    "Noise floor (dB)",      "Noise knee (dB)",
    "Upward attack (ms)",    "Upward release (ms)",
    "Duck attack (ms)",      "Duck release (ms)",
    "Detector mode",         "Downward enable",       "Bypass",
    "Architecture mode",     "Makeup follows dialog",
};

/* Ranges are deliberately generous: the engine clamps what it needs to, and a
 * too-tight hint here would silently cut off settings the presets use.
 *
 * The default matters more than it looks. LADSPA cannot express an arbitrary
 * default, only a handful of fixed hints - and "middle of the range" turned
 * Bypass (0..1) into 0.5, which rounds to 1: the compressor then sat in bypass
 * and nothing any other control did was audible. So every switch gets an
 * explicit hint. */
#define DEF_MIN  LADSPA_HINT_DEFAULT_MINIMUM
#define DEF_MID  LADSPA_HINT_DEFAULT_MIDDLE
#define DEF_MAX  LADSPA_HINT_DEFAULT_MAXIMUM
#define DEF_LOW  LADSPA_HINT_DEFAULT_LOW
#define DEF_0    LADSPA_HINT_DEFAULT_0
#define DEF_1    LADSPA_HINT_DEFAULT_1

static const struct { float lo, hi; int deflt; int integer; } ctl_range[N_CTL] = {
        { -80.0f, 0.0f, DEF_MID, 0 },  /* threshold        */
        { 1.0f, 20.0f, DEF_MIN, 0 },  /* ratio            */
        { 0.1f, 500.0f, DEF_LOW, 0 },  /* attack           */
        { 1.0f, 5000.0f, DEF_LOW, 0 },  /* release          */
        { 0.0f, 2000.0f, DEF_MIN, 0 },  /* hold             */
        { 0.0f, 30.0f, DEF_MID, 0 },  /* knee             */
        { -24.0f, 24.0f, DEF_MID, 0 },  /* makeup           */
        { 0.0f, 1.0f, DEF_MAX, 0 },  /* wet/dry          */
        { 1.0f, 1000.0f, DEF_LOW, 0 },  /* rms window       */
        { 0.0f, 1000.0f, DEF_LOW, 0 },  /* sc highpass      */
        { 0.0f, 50.0f, DEF_MIN, 0 },  /* lookahead        */
        { 0.0f, 40.0f, DEF_MID, 0 },  /* max gain         */
        { -60.0f, 0.0f, DEF_MID, 0 },  /* down threshold   */
        { 1.0f, 20.0f, DEF_MIN, 0 },  /* down ratio       */
        { -90.0f, 0.0f, DEF_MID, 0 },  /* atmo threshold   */
        { 0.0f, 40.0f, DEF_LOW, 0 },  /* atmo max gain    */
        { 0.0f, 30.0f, DEF_MID, 0 },  /* atmo knee        */
        { -90.0f, 0.0f, DEF_MID, 0 },  /* dialog threshold */
        { 0.0f, 40.0f, DEF_LOW, 0 },  /* dialog max gain  */
        { 0.0f, 30.0f, DEF_MID, 0 },  /* dialog knee      */
        { -120.0f, 0.0f, DEF_MID, 0 },  /* noise floor      */
        { 0.0f, 30.0f, DEF_MID, 0 },  /* noise knee       */
        { 0.1f, 500.0f, DEF_LOW, 0 },  /* upward attack    */
        { 1.0f, 5000.0f, DEF_LOW, 0 },  /* upward release   */
        { 0.1f, 500.0f, DEF_LOW, 0 },  /* duck attack      */
        { 1.0f, 5000.0f, DEF_LOW, 0 },  /* duck release     */
        { 0.0f, 2.0f, DEF_MIN, 1 },  /* detector mode    */
        { 0.0f, 1.0f, DEF_MIN, 1 },  /* downward enable  */
        { 0.0f, 1.0f, DEF_MIN, 1 },  /* bypass           */
        { 0.0f, 2.0f, DEF_MAX, 1 },  /* architecture     */
        { 0.0f, 1.0f, DEF_MIN, 1 },  /* makeup follows   */
};

/* Port layout: n audio in, n audio out, then all controls. */
static LADSPA_PortDescriptor  port_desc[3][2 * ENG_CH + N_CTL];
static LADSPA_PortRangeHint   port_hint[3][2 * ENG_CH + N_CTL];
static char                  *port_name[3][2 * ENG_CH + N_CTL];


/* ---------------------------------------------------------- live control --
 *
 * ffmpeg's LADSPA host does not forward runtime commands: an asendcmd to the
 * filter changes nothing (measured - the level stayed identical to the last
 * digit). Control ports are therefore frozen at whatever the filter graph was
 * built with, which would make a menu useless.
 *
 * So the plugin reads its own settings, the same way the grain and black level
 * scripts do: mpv writes a small file, a watcher thread picks it up and pushes
 * the values into the engine's atomics. No filter rebuild, no audio glitch.
 *
 * Format, one per line:   <param-index> <value>
 * A line "bypass 1" is accepted as a convenience alias.
 */
#define PARAMFILE_ENV "CINECOMP_PARAMS"
#define PARAMFILE_DEF "/.config/mpv/mpv-cinecomp-params"

static pthread_t        watch_thread;
static int              watch_running;
static int              watch_users;      /* live instances sharing the thread */
static pthread_mutex_t  watch_lock = PTHREAD_MUTEX_INITIALIZER;
static char             param_path[512];

static void apply_param_file(void)
{
    FILE *f = fopen(param_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        double val;
        if (sscanf(line, "%63s %lf", key, &val) != 2) continue;
        int idx = -1;
        if (key[0] >= '0' && key[0] <= '9') {
            idx = atoi(key);
        } else if (!strcmp(key, "bypass")) {
            idx = PARAM_BYPASS;
        }
        if (idx < 0 || idx >= N_CTL) continue;
        if (idx < PARAM__FLOAT_COUNT)
            engine_set_param_f((engine_param_t)idx, (float)val);
        else
            engine_set_param_i((engine_param_t)idx, (int)(val + 0.5));
    }
    fclose(f);
}

static void *watch_fn(void *arg)
{
    (void)arg;
    struct stat st;
    time_t last = 0;
    long last_ns = 0;
    while (watch_running) {
        if (stat(param_path, &st) == 0 &&
            (st.st_mtime != last || st.st_mtim.tv_nsec != last_ns)) {
            last = st.st_mtime;
            last_ns = st.st_mtim.tv_nsec;
            apply_param_file();
        }
        usleep(100000);      /* 100 ms is far below what a hand can turn */
    }
    return NULL;
}

static void watch_start(void)
{
    pthread_mutex_lock(&watch_lock);
    watch_users++;
    if (watch_running) { pthread_mutex_unlock(&watch_lock); return; }
    const char *env = getenv(PARAMFILE_ENV);
    if (env && *env) {
        snprintf(param_path, sizeof param_path, "%s", env);
    } else {
        const char *home = getenv("HOME");
        snprintf(param_path, sizeof param_path, "%s%s",
                 home ? home : "/tmp", PARAMFILE_DEF);
    }
    apply_param_file();          /* initial state before the first block */
    watch_running = 1;
    if (pthread_create(&watch_thread, NULL, watch_fn, NULL) != 0)
        watch_running = 0;
    pthread_mutex_unlock(&watch_lock);
}

/* Ohne das hier stirbt der Host: der Host entlaedt die Bibliothek beim
 * Entfernen des Filters, und ein noch laufender Thread rechnet dann in
 * Speicher, den es nicht mehr gibt. Gemessen als Speicherzugriffsfehler in
 * mpv, sobald der Kompressor im Menue abgeschaltet wurde. */
static void watch_stop(void)
{
    pthread_mutex_lock(&watch_lock);
    if (--watch_users > 0 || !watch_running) {
        pthread_mutex_unlock(&watch_lock);
        return;
    }
    watch_running = 0;
    pthread_mutex_unlock(&watch_lock);
    pthread_join(watch_thread, NULL);
}

/* ------------------------------------------------------------- life cycle */

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    cinecomp_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    /* ports = nch in + nch out + controls */
    c->nch = (unsigned)((d->PortCount - N_CTL) / 2);
    if (c->nch > ENG_CH) c->nch = ENG_CH;
    engine_set_sample_rate((unsigned)rate);
    watch_start();
    return c;
}

static void connect_port(LADSPA_Handle h, unsigned long port, LADSPA_Data *data)
{
    cinecomp_t *c = h;
    unsigned n = c->nch;
    if (port < n)                 c->in[port]        = data;
    else if (port < 2 * n)        c->out[port - n]   = data;
    else if (port < 2 * n + N_CTL) c->ctl[port - 2*n] = data;
}

static void run(LADSPA_Handle h, unsigned long nframes)
{
    cinecomp_t *c = h;
    if (nframes > MAX_BLOCK) nframes = MAX_BLOCK;

    /* Control ports are the starting point only. They cannot change while the
     * graph runs (see above), and re-applying them every block would fight the
     * watcher thread - so they are taken once, on the first block. */
    if (!c->ctl_taken) {
        for (int i = 0; i < PARAM__FLOAT_COUNT; i++)
            if (c->ctl[i]) engine_set_param_f((engine_param_t)i, (float)*c->ctl[i]);
        for (int i = PARAM__FLOAT_COUNT; i < N_CTL; i++)
            if (c->ctl[i]) engine_set_param_i((engine_param_t)i, (int)(*c->ctl[i] + 0.5f));
        apply_param_file();      /* the file wins over the port defaults */
        c->ctl_taken = 1;
    }

    /* The engine always works on eight channels. Anything the host did not
     * connect reads silence and writes to scratch, so the linked side-chain
     * still sees a complete picture. */
    const float *in[ENG_CH];
    float *out[ENG_CH];
    for (unsigned ch = 0; ch < ENG_CH; ch++) {
        in[ch]  = (ch < c->nch && c->in[ch])  ? c->in[ch]  : silence;
        out[ch] = (ch < c->nch && c->out[ch]) ? c->out[ch] : scratch[ch];
    }
    engine_process_block(in, out, (unsigned)nframes);
}

static void cleanup(LADSPA_Handle h)
{
    watch_stop();
    free(h);
}

/* ------------------------------------------------------------- descriptor */

static LADSPA_Descriptor desc[3];

static void build(int v, unsigned nch, unsigned long id, const char *label,
                  const char *name)
{
    const unsigned np = 2 * nch + N_CTL;
    for (unsigned i = 0; i < nch; i++) {
        char b[32];
        port_desc[v][i] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
        snprintf(b, sizeof b, "In %u", i + 1);
        port_name[v][i] = strdup(b);
        port_desc[v][nch + i] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
        snprintf(b, sizeof b, "Out %u", i + 1);
        port_name[v][nch + i] = strdup(b);
    }
    for (unsigned i = 0; i < N_CTL; i++) {
        unsigned p = 2 * nch + i;
        port_desc[v][p] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
        port_name[v][p] = strdup(ctl_name[i]);
        port_hint[v][p].LowerBound   = ctl_range[i].lo;
        port_hint[v][p].UpperBound   = ctl_range[i].hi;
        port_hint[v][p].HintDescriptor =
            LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
            ctl_range[i].deflt |
            (ctl_range[i].integer ? LADSPA_HINT_INTEGER : 0);
    }
    desc[v].UniqueID   = id;
    desc[v].Label      = label;
    desc[v].Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE;
    desc[v].Name       = name;
    desc[v].Maker      = "cinecomp";
    desc[v].Copyright  = "MIT";
    desc[v].PortCount  = np;
    desc[v].PortDescriptors = port_desc[v];
    desc[v].PortNames  = (const char *const *)port_name[v];
    desc[v].PortRangeHints  = port_hint[v];
    desc[v].instantiate     = instantiate;
    desc[v].connect_port    = connect_port;
    desc[v].run             = run;
    desc[v].cleanup         = cleanup;
}

static void __attribute__((constructor)) init(void)
{
    build(0, 2,      0x636E6331, "cinecomp_stereo", "cinecomp upward (stereo)");
    build(1, 6,      0x636E6336, "cinecomp_51",     "cinecomp upward (5.1)");
    build(2, ENG_CH, 0x636E6338, "cinecomp_71",     "cinecomp upward (7.1)");
}

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    return (index < 3) ? &desc[index] : NULL;
}
