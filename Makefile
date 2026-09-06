# cinecomp standalone build
#
# Builds a single-binary native GUI app: GLFW+OpenGL window with
# Dear ImGui control surface, JACK audio engine + OSC server in the
# background.
#
# Build deps (Linux): jack2 (or pipewire-jack-pulseaudio shims), GLFW3,
#   libGL, X11 dev headers. On Debian/Ubuntu:
#     apt install build-essential pkg-config libjack-jackd2-dev libglfw3-dev libgl-dev
#   On Arch:
#     pacman -S base-devel pkgconf jack2 glfw mesa
#
# Run:  ./cinecomp
#        ./cinecomp --name comp2 --osc 14042   # second instance

CC      ?= gcc
CXX     ?= g++
PKG     ?= pkg-config

CFLAGS  := -O3 -ffast-math -funroll-loops -Wall -Wextra -fPIC
CXXFLAGS := -O2 -fPIC -std=c++17 -Wall -Wno-unused-parameter
CPPFLAGS := -Isrc -Ivendor/imgui -Ivendor/imgui/backends \
            $(shell $(PKG) --cflags jack glfw3)

LDFLAGS  := -lm -lpthread -ldl \
            $(shell $(PKG) --libs jack glfw3) \
            -lGL

IMGUI_DIR := vendor/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp \
             $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

IMGUI_OBJ := $(IMGUI_SRC:.cpp=.o)

ENGINE_OBJ := src/audio_engine.o
GUI_OBJ    := src/gui.o

ALL_OBJ := $(ENGINE_OBJ) $(GUI_OBJ) $(IMGUI_OBJ)

TARGET := cinecomp

all: $(TARGET)

$(TARGET): $(ALL_OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(ALL_OBJ)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run

# ---------------------------------------------------------------- LADSPA --
# The plugin builds the same engine without JACK, so mpv (or any LADSPA host)
# can run the compressor with no server and no patchbay:
#   af=ladspa=file=.../cinecomp_ladspa.so:plugin=cinecomp_stereo
LADSPA_SO := cinecomp_ladspa.so
LADSPA_SRC := src/ladspa_cinecomp.c src/audio_engine.c

$(LADSPA_SO): $(LADSPA_SRC) src/audio_engine.h vendor/ladspa.h
	$(CC) -O3 -ffast-math -funroll-loops -Wall -Wextra -fPIC -shared \
	      -DCINECOMP_NO_JACK -o $@ $(LADSPA_SRC) -lm -lpthread

ladspa: $(LADSPA_SO)

install-ladspa: $(LADSPA_SO)
	install -d $(HOME)/.ladspa
	install -m 755 $(LADSPA_SO) $(HOME)/.ladspa/
