#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# From a fresh clone to a built server, in one command:
#
#   1. the system packages a build needs (compiler, CMake, Ninja, the libraries, a JDK for mcgen);
#   2. Mojang's server jar, and the tables generated from it (tools/mcgen/mcgen.py setup) - they are
#      Mojang's data and are not in this repository, so every clone makes its own;
#   3. a release build.
#
# Outside of this directory it changes one thing: it installs packages with the system's package
# manager. Run it again whenever you like - every step skips what is already there.
set -euo pipefail
cd "$(dirname "$0")"

JDK_MAJOR=25 # what Minecraft's jar of DEFAULT_VERSION (tools/mcgen/mcgen.py) is compiled for
PRESET=release
YES=0 DEPS=1 BUILD=1

usage() {
    cat <<'EOF'
usage: ./install.sh [--yes] [--no-deps] [--no-build] [--preset NAME]

  --yes, -y       do not ask before installing packages and downloading Minecraft's server jar
  --no-deps       install no packages (everything is there already, or you install it yourself)
  --no-build      stop after the generated tables: no cmake
  --preset NAME   the CMake preset to build (default: release; see CMakePresets.json)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
    -y | --yes) YES=1 ;;
    --no-deps) DEPS=0 ;;
    --no-build) BUILD=0 ;;
    --preset)
        [ $# -ge 2 ] || { usage >&2; exit 2; }
        PRESET=$2
        shift
        ;;
    --preset=*) PRESET=${1#--preset=} ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------- the machine
[ "$(uname -s)" = Linux ] || die "Linux only: the network layer is io_uring."
[ "$(uname -m)" = x86_64 ] || die "x86-64 only for now: the hot paths use AVX2, BMI2 and AES-NI."

kernel=$(uname -r)
kmajor=${kernel%%.*}
[ "$kmajor" -ge 6 ] 2>/dev/null || die "kernel $kernel: 6.0 or newer is needed (io_uring provided-buffer rings)."

flags=" $(grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2) "
missing=""
for f in avx2 bmi1 bmi2 fma movbe aes pclmulqdq; do
    case "$flags" in *" $f "*) ;; *) missing="$missing $f" ;; esac
done
[ -z "$missing" ] || die "this CPU lacks:$missing. The baseline is x86-64-v3 (Haswell / Zen 1 and newer) with AES-NI."

if [ "$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo 0)" != 0 ]; then
    echo "install.sh: warning: io_uring is disabled on this machine (sysctl kernel.io_uring_disabled): the server will build but not start." >&2
fi

# ---------------------------------------------------------------------------- what is about to happen
# Package names per family. A JDK is needed by mcgen only (javap, and the three steps that run the jar).
ARCH_PKGS="gcc cmake ninja pkgconf python liburing libdeflate openssl curl yyjson gtest jdk${JDK_MAJOR}-openjdk"
FEDORA_PKGS="gcc-c++ cmake ninja-build pkgconf-pkg-config python3 liburing-devel libdeflate-devel openssl-devel libcurl-devel yyjson-devel gtest-devel java-${JDK_MAJOR}-openjdk-devel"
DEBIAN_PKGS="g++ cmake ninja-build pkg-config python3 ca-certificates liburing-dev libdeflate-dev libssl-dev libcurl4-openssl-dev libyyjson-dev libgtest-dev openjdk-${JDK_MAJOR}-jdk-headless"

family=none
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case " ${ID:-} ${ID_LIKE:-} " in
    *" arch "*) family=arch ;;
    *" fedora "*) family=fedora ;;
    *" debian "* | *" ubuntu "*) family=debian ;;
    esac
fi
case $family in
arch) pkgs=$ARCH_PKGS ;;
fedora) pkgs=$FEDORA_PKGS ;;
debian) pkgs=$DEBIAN_PKGS ;;
none) pkgs="" ;;
esac

sudo=""
[ "$(id -u)" -eq 0 ] || sudo=sudo

echo "This will:"
if [ $DEPS -eq 1 ] && [ $family != none ]; then
    echo "  - install these packages${sudo:+ (with sudo)}, unless they are there already:"
    echo "      $pkgs"
elif [ $DEPS -eq 1 ]; then
    echo "  - install NO packages: this distribution is not one install.sh knows. What a build needs:"
    echo "      a C++23 compiler with #embed (GCC 15+ or Clang 19+; Elaine is developed with GCC 16), CMake 3.25+, Ninja,"
    echo "      pkg-config, Python 3, JDK ${JDK_MAJOR}+, liburing 2.4+, libdeflate, OpenSSL, libcurl, yyjson, GoogleTest"
fi
echo "  - download the Minecraft server jar from Mojang (piston-data.mojang.com, sha1-checked) into .cache/ and run"
echo "    the data generator inside it. It is Mojang's software under Mojang's terms: https://aka.ms/MinecraftEULA"
echo "    No server is started and nothing of it leaves this directory."
[ $BUILD -eq 0 ] || echo "  - build the '$PRESET' preset into build/$PRESET/"

if [ $YES -eq 0 ]; then
    [ -t 0 ] || die "not a terminal: run it with --yes to go ahead without being asked."
    printf 'Go on? [y/N] '
    read -r answer
    case "$answer" in y | Y | yes | YES) ;; *) die "stopped; nothing was changed." ;; esac
fi

# ---------------------------------------------------------------------------- packages
if [ $DEPS -eq 1 ] && [ $family != none ]; then
    say "Packages"
    if [ -n "$sudo" ] && ! command -v sudo >/dev/null; then
        die "no sudo: install the packages above as root, then run ./install.sh --no-deps"
    fi
    # shellcheck disable=SC2086  # the lists are words
    case $family in
    arch)
        # No -y here: refreshing the database without upgrading is how an Arch system breaks.
        $sudo pacman -S --needed --noconfirm $pkgs ||
            die "pacman failed. If it could not find a package, the database is stale: sudo pacman -Syu, then again."
        ;;
    fedora) $sudo dnf install -y $pkgs ;;
    debian)
        $sudo apt-get update
        DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-install-recommends $pkgs
        ;;
    esac
fi

for tool in python3 cmake ninja pkg-config; do
    command -v $tool >/dev/null || die "$tool is not installed."
done

# ---------------------------------------------------------------------------- the compiler
# The one thing that is commonly too old: #embed (C++26, taken as an extension in C++23 mode).
say "Compiler"
cxx=${CXX:-c++}
command -v "$cxx" >/dev/null || die "no C++ compiler ($cxx). Set CXX to one."
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
printf 'x' >"$probe/one.bin"
cat >"$probe/probe.cpp" <<'EOF'
static const unsigned char kOne[] = {
#embed "one.bin"
};
int main() { return kOne[0] == 'x' ? 0 : 1; }
EOF
if ! "$cxx" -std=gnu++23 -march=x86-64-v3 -maes -mpclmul -o "$probe/probe" "$probe/probe.cpp" 2>"$probe/err"; then
    sed 's/^/    /' "$probe/err" | head -5 >&2
    die "$("$cxx" --version | head -1) cannot compile '#embed' for x86-64-v3. GCC 15+ or Clang 19+ is needed
            (Elaine is developed with GCC 16); set CXX to a newer compiler and run this again."
fi
"$probe/probe" || die "the compiler's '#embed' gives wrong bytes."
"$cxx" --version | head -1

# ---------------------------------------------------------------------------- Mojang's jar -> the generated tables
say "Generated tables (tools/mcgen/mcgen.py setup)"
python3 tools/mcgen/mcgen.py setup

# ---------------------------------------------------------------------------- the build
if [ $BUILD -eq 0 ]; then
    say "Done: the tables are there. To build:  cmake --preset $PRESET && cmake --build --preset $PRESET"
    exit 0
fi
say "Build ($PRESET)"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

version=$(sed -n 's/^DEFAULT_VERSION = "\(.*\)"$/\1/p' tools/mcgen/mcgen.py)
exe=$(sed -n 's/^set(MC_EXE "\([^"]*\)".*/\1/p' CMakeLists.txt)
say "Done"
cat <<EOF
The server:   build/$PRESET/$exe
Try it:       build/$PRESET/$exe --online-mode=false     # then join localhost with an unmodified Minecraft $version client
Its tests:    ctest --preset release                     # (and the dev preset: sanitizers - see CONTRIBUTING.md)
What next:    README.md, CONTRIBUTING.md
EOF
