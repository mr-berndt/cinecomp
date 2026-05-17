# filmcomp — Standalone

Self-contained Linux-Desktop-Build des Upward-Compressors für
Filmbetrieb mit nativem GUI (Dear ImGui + GLFW). Läuft als regulärer
JACK-Client, ist also über Carla/Catia/qjackctl in DAW-Workflows
einbindbar.

Architektur, DSP-Algorithmus und Default-Presets stammen 1:1 vom
aroio6-Buildroot-Plugin `aroio_filmcomp` (Aroio-Plattform-Variante des
gleichen Engines). Drei Architektur-Modi sind verfügbar:

| Mode | Topologie |
|---|---|
| **Classic** | Eine Upward-Stage + Duck (klassischer Compressor) |
| **Zonal v1** | Summierte Atmo + Dialog Upward-Stages + Duck |
| **Zonal v2** | Band-shaped Plateaus pro Zone + asymmetrische Envelope (Default seit 2026-05-15) |

Die v2-Plateau-Topologie ist auf den Cinema-Härtetest-Szenen
verifiziert (John-Wick Doppel-Salve, A-Quiet-Place Wecker + Bastel,
Boot auf hoher See). Sie pumpt nicht, lässt Transienten unangetastet
durch, transparenter Atmo-Lift bis +24 dB.

## Build

**Abhängigkeiten (Linux):**

```
# Debian/Ubuntu
sudo apt install build-essential pkg-config libjack-jackd2-dev libglfw3-dev libgl-dev

# Arch/Manjaro
sudo pacman -S base-devel pkgconf jack2 glfw mesa

# Fedora
sudo dnf install gcc-c++ make pkgconf jack-audio-connection-kit-devel glfw-devel mesa-libGL-devel
```

**Bauen:**

```
make
```

Output: einzelne ~900 KB ELF-Datei `filmcomp` im Root des Projekts.

## Starten

```
./filmcomp                              # Defaults
./filmcomp --name comp2 --osc 14042     # Zweite Instanz
./filmcomp --state /pfad/zu/preset.ini  # Custom Preset-File
```

Optionen:

| Flag | Default | Zweck |
|---|---|---|
| `--name <s>` | `filmcomp` | JACK-Client-Name |
| `--osc <port>` | `14041` | OSC-Server-Port (UDP) |
| `--state <path>` | `$XDG_CONFIG_HOME/filmcomp/state.ini` | Persistenz |

Auto-Save: bei Slider-Änderungen ~1 s Debounce + finaler Save beim
Beenden.

## JACK-Ports

8 Inputs / 8 Outputs (7.1-Konvention):
```
in_L  in_R  in_C  in_LFE  in_LS  in_RS  in_RBL  in_RBR
out_L out_R out_C out_LFE out_LS out_RS out_RBL out_RBR
```

Layout-agnostisch via smooth Channel-Weights: stille Ports werden
automatisch aus der Detection rausgehalten. Bei Stereo-Workflows
einfach in_L+in_R verdrahten, der Rest bleibt offen.

## OSC

Vollständige API kompatibel zur aroio6-Buildroot-Variante, gleiche
Pfade. Hauptcontroller:

```
/filmcomp/bypass i             — 0/1
/filmcomp/architecture i       — 0=Classic, 1=v1, 2=v2
/filmcomp/detector i           — 0=RMS, 1=Peak, 2=Dual (zonal-only)

# Classic-Stage
/filmcomp/threshold f          — dB (-60..0)
/filmcomp/ratio f              — 1..20
/filmcomp/max_gain f           — dB (0..30)

# Zonal-Stages (v1 + v2)
/filmcomp/atmo/threshold f     /filmcomp/atmo/max_gain f    /filmcomp/atmo/knee f
/filmcomp/dialog/threshold f   /filmcomp/dialog/max_gain f  /filmcomp/dialog/knee f
/filmcomp/noise/floor f        /filmcomp/noise/knee f
/filmcomp/upward/attack_ms f   /filmcomp/upward/release_ms f
/filmcomp/duck/attack_ms f     /filmcomp/duck/release_ms f

# Presets
/filmcomp/preset/select i      — 0=LOW, 1=MID, 2=HIGH
/filmcomp/preset/save i        — save current params to slot N
/filmcomp/preset/reset i       — reset slot N to factory defaults

# Meter-Subscribe
/filmcomp/subscribe            — UDP-Meter-Broadcast aktivieren
/filmcomp/unsubscribe
/filmcomp/get
```

Volle Liste siehe `src/audio_engine.c` (Doc-Kommentar am Anfang).

## Default-Werte (MID-Preset, Boot)

Werte aus der aroio6-Reference-Session 2026-05-15 (cinema-tuned, zonal
v2 Plateau-Topologie):

```
Architecture: zonal v2     Detector: Peak           Bypass: ON

Atmo:    thr -35 / lift +24 / knee 20
Dialog:  thr -10.5 / lift +10 / knee 13.5
Noise:   floor -70.5 / knee 10.5
Up-Env:  attack 451 ms / release 20 ms        (asymmetrisch: slow rise / fast fall)
Duck:    thr -14 / ratio 1.8 / knee 16.5 / attack 9 / release 51   (LOW: off)

Classic-Stage (für arch=Classic-Fallback):
         thr -10 / ratio 4 / max_gain 10 / makeup 0
```

Beim ersten Start (kein State-File) werden die MID-Werte gesetzt.
Spätere Änderungen werden in der state.ini persistiert.

## Lizenzen

- filmcomp Code: proprietär (Abacus Electronics)
- Dear ImGui: MIT — `vendor/imgui/LICENSE.txt`
- GLFW: zlib/libpng — Distributions-Paket
