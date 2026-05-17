# filmcomp standalone build
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
# Run:  ./filmcomp
#        ./filmcomp --name comp2 --osc 14042   # second instance

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

TARGET := filmcomp

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
