# aroio_filmcomp — Standalone

Self-contained Linux-Desktop-Build des aroio_filmcomp Upward-Compressors
mit nativem GUI (Dear ImGui + GLFW). Läuft als regulärer JACK-Client,
ist also über Carla/Catia/qjackctl in DAW-Workflows einbindbar.

Architektur, DSP-Algorithmus und Default-Preset sind 1:1 die aroio6-
Variante — siehe `../../docs/FILMCOMP-PARAMETER.md` für die
Parameter-Theorie.

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

Output: einzelne ~900 KB ELF-Datei `aroio_filmcomp` im Root des
Projekts.

## Starten

```
./aroio_filmcomp                              # Defaults
./aroio_filmcomp --name comp2 --osc 14042     # Zweite Instanz
./aroio_filmcomp --state /pfad/zu/preset.ini  # Custom Preset-File
```

Optionen:

| Flag | Default | Zweck |
|---|---|---|
| `--name <s>` | `filmcomp` | JACK-Client-Name |
| `--osc <port>` | `14041` | OSC-Server-Port (UDP) |
| `--state <path>` | `$XDG_CONFIG_HOME/aroio_filmcomp/state.ini` | Persistenz |

Auto-Save: bei Slider-Änderungen ~1 s Debounce + finaler Save beim
Beenden.

## JACK-Ports

8 Inputs / 8 Outputs (7.1-konvention):
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
/filmcomp/bypass i           — 0/1
/filmcomp/threshold f        — dB (-60..0)
/filmcomp/ratio f            — 1..20
/filmcomp/max_gain f         — dB (0..30)
/filmcomp/detector i         — 0=RMS, 1=Peak
/filmcomp/subscribe          — UDP-Meter-Broadcast aktivieren
```

Volle Liste siehe `src/audio_engine.c` (Doc-Kommentar am Anfang).

## Mid-Preset (Default)

Werte aus der aroio6-Reference-Session vom 2026-05-12 (verifiziert
gegen Spielfilm-Material — Doppel-Salve + alarm-clock-ticks-over-atmo):

```
threshold  -10.5 dB    ratio       3:1       max_gain   10 dB
attack       5  ms     release    10 ms      hold        0 ms
knee         6  dB     makeup    -0.5 dB     wet/dry     1.0
rms_win    500  ms     sc_hpf     60 Hz      lookahead  15 ms
detector  Peak         bypass    ON
```

Beim ersten Start (kein State-File) werden diese gesetzt. Spätere
Änderungen werden in der state.ini persistiert.

## Lizenzen

- aroio_filmcomp Code: proprietär (Abacus Electronics)
- Dear ImGui: MIT — `vendor/imgui/LICENSE.txt`
- GLFW: zlib/libpng — Distributions-Paket
