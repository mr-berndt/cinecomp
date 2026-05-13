#!/bin/bash
# Build a .deb package for aroio_filmcomp standalone.
#
# Output: dist/aroio-filmcomp_<version>_amd64.deb
#
# Run as user (no sudo needed). Builds inside the existing aroio6
# docker container so the host doesn't need libjack/glfw dev pkgs.
# If you have the dev packages installed natively, you can also
# invoke this directly outside Docker:  BUILD_NATIVE=1 ./build-deb.sh
set -euo pipefail

VERSION="${VERSION:-1.0.0}"
PKGROOT="$(dirname "$(readlink -f "$0")")"
cd "$PKGROOT"

DIST="$PKGROOT/dist"
STAGE="$PKGROOT/dist/stage"
DEB="$DIST/aroio-filmcomp_${VERSION}_amd64.deb"

rm -rf "$STAGE"
mkdir -p "$DIST" "$STAGE/usr/local/bin" "$STAGE/DEBIAN" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/doc/aroio-filmcomp"

# --- Build the binary ---------------------------------------------------------
if [[ "${BUILD_NATIVE:-0}" == "1" ]]; then
    make clean
    make
else
    # Use the aroio6 build container with apt-installed deps.
    DOCKER_IMAGE="${DOCKER_IMAGE:-aroio6-builder}"
    docker run --rm -u 0:0 \
        -v "$PKGROOT":/build \
        "$DOCKER_IMAGE" bash -c "
            apt-get update -qq >/dev/null &&
            apt-get install -y -qq build-essential libjack-jackd2-dev libglfw3-dev libgl-dev pkg-config >/dev/null &&
            cd /build && make clean && make &&
            chown $(id -u):$(id -g) aroio_filmcomp src/*.o vendor/imgui/*.o vendor/imgui/backends/*.o 2>/dev/null || true
        "
fi

# --- Stage files --------------------------------------------------------------
install -m 0755 aroio_filmcomp "$STAGE/usr/local/bin/aroio_filmcomp"
install -m 0644 README.md      "$STAGE/usr/share/doc/aroio-filmcomp/README.md"

# Desktop entry so DAWs / file managers / app launchers see it.
cat > "$STAGE/usr/share/applications/aroio-filmcomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=aroio_filmcomp
GenericName=Upward Compressor for Film
Comment=JACK-based upward dynamic range compressor for film playback
Exec=/usr/local/bin/aroio_filmcomp
Icon=audio-x-generic
Terminal=false
Categories=AudioVideo;Audio;
Keywords=audio;jack;compressor;dynamics;film;
EOF

# --- Control file -------------------------------------------------------------
SIZE_KB=$(du -sk "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: aroio-filmcomp
Version: $VERSION
Section: sound
Priority: optional
Architecture: amd64
Installed-Size: $SIZE_KB
Depends: libjack-jackd2-0 | libjack0, libglfw3, libgl1
Maintainer: Abacus Electronics <avm-project@humboldtforum.org>
Description: aroio_filmcomp — Upward compressor for film playback
 Self-contained JACK client with native Dear ImGui control surface.
 Lifts quiet program material (dialog, ambience) without touching loud
 transients. Layout-agnostic 8-channel side-chain detection (Stereo,
 5.1, 7.1 all work without configuration). Peak detector with
 configurable lookahead so transients pass through cleanly.
 .
 Mid preset (default on first run) is tuned against film material with
 isolated transients (gunshots, alarm clocks over quiet atmo). Settings
 persist to \$XDG_CONFIG_HOME/aroio_filmcomp/state.ini.
EOF

# --- Build the .deb -----------------------------------------------------------
if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
    # Fall back to the docker container's dpkg-deb if the host doesn't
    # have one (debian/ubuntu hosts usually do; arch / fedora don't).
    docker run --rm -u 0:0 \
        -v "$PKGROOT":/build \
        "${DOCKER_IMAGE:-aroio6-builder}" bash -c "
            cd /build &&
            dpkg-deb --build --root-owner-group dist/stage $(basename "$DEB") &&
            mv $(basename "$DEB") dist/ &&
            chown $(id -u):$(id -g) dist/$(basename "$DEB")
        "
fi

rm -rf "$STAGE"

echo
echo "Built: $DEB"
ls -lh "$DEB"
echo
echo "Install:"
echo "  sudo dpkg -i $DEB"
echo "  sudo apt-get install -f   # if missing deps"
