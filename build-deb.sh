#!/bin/bash
# Build a .deb package for the cinecomp standalone.
#
# Output: dist/cinecomp_<version>_amd64.deb
#
# Run as user (no sudo needed). Builds inside a debian:12 (bookworm,
# glibc 2.36) docker container so the host needs no libjack/glfw dev
# pkgs and the binary stays portable: glibc 2.36 runs on bookworm and
# every newer Debian/Ubuntu (convolver, muaddib, ...). Override with
# DOCKER_IMAGE=... if you need a different base.
# If you have the dev packages installed natively, you can also
# invoke this directly outside Docker:  BUILD_NATIVE=1 ./build-deb.sh
set -euo pipefail

VERSION="${VERSION:-1.1.0}"
PKGROOT="$(dirname "$(readlink -f "$0")")"
cd "$PKGROOT"

DIST="$PKGROOT/dist"
STAGE="$PKGROOT/dist/stage"
DEB="$DIST/cinecomp_${VERSION}_amd64.deb"

rm -rf "$STAGE"
mkdir -p "$DIST" "$STAGE/usr/local/bin" "$STAGE/DEBIAN" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/doc/cinecomp"

# --- Build the binary ---------------------------------------------------------
if [[ "${BUILD_NATIVE:-0}" == "1" ]]; then
    make clean
    make
else
    # debian:12 = glibc 2.36 baseline → portable binary. Deps apt-installed
    # in the throwaway container; image is pulled on first run.
    DOCKER_IMAGE="${DOCKER_IMAGE:-debian:12}"
    docker run --rm -u 0:0 \
        -v "$PKGROOT":/build \
        "$DOCKER_IMAGE" bash -c "
            apt-get update -qq >/dev/null &&
            apt-get install -y -qq build-essential libjack-jackd2-dev libglfw3-dev libgl-dev pkg-config >/dev/null &&
            cd /build && make clean && make &&
            chown $(id -u):$(id -g) cinecomp src/*.o vendor/imgui/*.o vendor/imgui/backends/*.o 2>/dev/null || true
        "
fi

# --- Stage files --------------------------------------------------------------
install -m 0755 cinecomp        "$STAGE/usr/local/bin/cinecomp"
install -m 0644 README.md       "$STAGE/usr/share/doc/cinecomp/README.md"

# Desktop entry so DAWs / file managers / app launchers see it.
cat > "$STAGE/usr/share/applications/cinecomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=cinecomp
GenericName=Upward Compressor for Film
Comment=JACK-based upward dynamic range compressor for film playback
Exec=/usr/local/bin/cinecomp
Icon=audio-x-generic
Terminal=false
Categories=AudioVideo;Audio;
Keywords=audio;jack;compressor;dynamics;film;
EOF

# --- Control file -------------------------------------------------------------
SIZE_KB=$(du -sk "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: cinecomp
Version: $VERSION
Section: sound
Priority: optional
Architecture: amd64
Installed-Size: $SIZE_KB
Depends: libjack-jackd2-0 | libjack0, libglfw3, libgl1
Maintainer: Abacus Electronics <avm-project@humboldtforum.org>
Description: cinecomp — Upward compressor for film playback
 Self-contained JACK client with native Dear ImGui control surface.
 Lifts quiet program material (dialog, ambience) without touching loud
 transients. Layout-agnostic 8-channel side-chain detection (Stereo,
 5.1, 7.1 all work without configuration). Peak detector with
 configurable lookahead so transients pass through cleanly.
 .
 Three architecture modes: classic single-stage; zonal v1 (summed
 atmo + dialog upward stages); zonal v2 (band-shaped plateaus, default
 since 2026-05-15 — pump-resistant on real cinema material).
 .
 Originated as the engine of the aroio6 Buildroot package
 \`aroio_filmcomp\`. Settings persist to \$XDG_CONFIG_HOME/cinecomp/state.ini.
EOF

# --- Build the .deb -----------------------------------------------------------
if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
    # Fall back to the docker container's dpkg-deb if the host doesn't
    # have one (debian/ubuntu hosts usually do; arch / fedora don't).
    docker run --rm -u 0:0 \
        -v "$PKGROOT":/build \
        "${DOCKER_IMAGE:-debian:12}" bash -c "
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
