#!/bin/sh
# build-on-odroid.sh -- build the whole deploy tarball ON the ODROID-N2+.
#
# Why a Debian bookworm chroot and not the board's own trixie: every recipe in
# this tree was measured with bookworm's toolchain (gcc 12, glibc 2.36 headers;
# tools/build-toolchain/Dockerfile is FROM debian:bookworm). Trixie's gcc 14
# makes implicit declarations hard errors, which DirectFB 1.4 is full of, and
# glibc >= 2.38 headers redirect strtol & co. to __isoc23_* symbols that the RX3's
# glibc 2.13 does not have -- objects that build fine and then fail to load. The
# chroot keeps the build identical to the Pi's. It costs ~400 MB in /srv.
#
# Usage (as root, from the repo root, with extracted/ in place -- docs/04, 21):
#   sh scripts/odroid/build-on-odroid.sh            # everything
#   sh scripts/odroid/build-on-odroid.sh dfb        # only DirectFB -> work/dfb
#   sh scripts/odroid/build-on-odroid.sh chroot     # only build-chroot.sh
#
# Result: work/rb-odroid.tgz (TARBALL_NAME overrides). Install it with
#   sh scripts/device/install.sh work/rb-odroid.tgz
#
# The DirectFB steps are tools/build-directfb/README.md's, verbatim, including
# its verification; read that file for why each flag is there.

set -eu

STEP="${1:-all}"
TARBALL_NAME="${TARBALL_NAME:-rb-odroid.tgz}"
BR="${RB_BUILD_ROOT:-/srv/rb-bookworm}"

# ---------------------------------------------------------------- inner part
# Runs inside the bookworm chroot, with the repo at /src.
if [ "${RB_IN_BUILD_CHROOT:-0}" = "1" ]; then
    cd /src
    RX3=/src/extracted/XDJRX3-rootfs
    W=/src/work
    mkdir -p "$W/build"

    if [ "$STEP" = "all" ] || [ "$STEP" = "dfb" ]; then
        echo "== [dfb 1/6] fluxcomp (native) =="
        cd "$W/build"
        [ -d flux ] || git clone https://github.com/deniskropp/flux.git flux
        cd flux
        git checkout -q 10ad2ebc78b396032714b839f200848ea0dd9503
        if [ ! -x src/fluxcomp ]; then
            autoreconf -fi
            env -u CC -u CFLAGS -u LDFLAGS CXX=g++ ./configure
            make -j"$(nproc)"
        fi
        PATH="$PWD/src:$PATH"; export PATH

        echo "== [dfb 2/6] DirectFB 1.4.16 source + patch =="
        cd "$W/build"
        rm -rf directfb
        git clone -q https://github.com/deniskropp/DirectFB.git directfb
        cd directfb
        git checkout -q 363739298335a0f76dea2de1c9bc523293242baa
        patch -p1 < /src/tools/build-directfb/directfb-full.diff

        echo "== [dfb 3/6] configure + make (soft-float, against the RX3 sysroot) =="
        CC=arm-linux-gnueabi-gcc
        CFLAGS="-march=armv5t -mfloat-abi=soft --sysroot=$RX3"
        LDFLAGS="--sysroot=$RX3 -L$RX3/lib -L$RX3/usr/lib -Wl,-rpath-link,$RX3/lib:$RX3/usr/lib"
        export CC CFLAGS LDFLAGS
        ./autogen.sh \
            --host=arm-linux-gnueabi \
            --prefix=/usr --libdir=/lib \
            --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
            --with-gfxdrivers=none \
            --disable-osx --disable-devmem
        make -j"$(nproc)"

        echo "== [dfb 4/6] stage into work/dfb =="
        rm -rf "$W/dfb"
        make install DESTDIR="$W/dfb"
        unset CC CFLAGS LDFLAGS

        echo "== [dfb 5/6] soname fix-up (.so.6 -> .so.0) =="
        cd "$W/dfb"
        for base in libdirect libfusion libdirectfb; do
            patchelf --set-soname "$base-1.4.so.0" "lib/$base-1.4.so.6.0.10"
            mv "lib/$base-1.4.so.6.0.10" "lib/$base-1.4.so.0.0.0"
            rm -f "lib/$base-1.4.so.6" "lib/$base-1.4.so"
            ln -sf "$base-1.4.so.0.0.0" "lib/$base-1.4.so.0"
        done
        for f in lib/lib*-1.4.so.0.0.0 lib/directfb-1.4-6/*/*.so; do
            for base in libdirect libfusion libdirectfb; do
                arm-linux-gnueabi-readelf -d "$f" | grep -q "\[$base-1.4.so.6\]" || continue
                patchelf --replace-needed "$base-1.4.so.6" "$base-1.4.so.0" "$f"
            done
        done

        echo "== [dfb 6/6] verify: only GLIBC_2.4/2.7, sonames .so.0 =="
        n=0; bad=0
        for f in lib/lib*-1.4.so.0.0.0 lib/directfb-1.4-6/*/*.so; do
            n=$((n + 1))
            v=$(arm-linux-gnueabi-objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tr '\n' ' ')
            printf '  %-40s %s\n' "$(basename "$f")" "$v"
            echo "$v" | grep -qE 'GLIBC_2\.([89]|[1-9][0-9])' && bad=1
            arm-linux-gnueabi-readelf -d "$f" | grep -q 'direct[a-z]*-1.4.so.6' && bad=1
        done
        echo "  $n objects checked"
        [ "$n" -ge 6 ] || { echo "too few DirectFB objects staged ($n)" >&2; exit 1; }
        [ "$bad" = "0" ] || { echo "a DirectFB object needs a glibc newer than 2.7 or still names .so.6" >&2; exit 1; }
        cd /src
    fi

    if [ "$STEP" = "all" ] || [ "$STEP" = "chroot" ]; then
        echo "== build-chroot.sh (shims + rbp patches + DirectFB + rootfs) =="
        RX3=/src/extracted DFB="$W/dfb" TARBALL_NAME="$TARBALL_NAME" bash scripts/build-chroot.sh
    fi
    exit 0
fi

# ---------------------------------------------------------------- host part
[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
[ -d "$REPO/extracted/XDJRX3-rootfs/lib" ] || {
    echo "no extracted/XDJRX3-rootfs in $REPO -- see docs/21 (fsck.cramfs --extract)" >&2
    exit 1
}

if [ ! -x "$BR/usr/bin/apt-get" ]; then
    echo "== creating the bookworm build chroot at $BR (one time) =="
    command -v debootstrap >/dev/null || apt-get install -y debootstrap
    debootstrap --variant=minbase bookworm "$BR" http://deb.debian.org/debian
fi
cp /etc/resolv.conf "$BR/etc/resolv.conf"

mkdir -p "$BR/src" "$BR/proc"
cleanup() {
    umount "$BR/src" 2>/dev/null || true
    umount "$BR/proc" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
mountpoint -q "$BR/proc" || mount -t proc proc "$BR/proc"
mountpoint -q "$BR/src"  || mount --bind "$REPO" "$BR/src"

if [ ! -x "$BR/usr/bin/arm-linux-gnueabi-gcc" ] || [ ! -x "$BR/usr/bin/patchelf" ]; then
    echo "== installing the toolchain in the chroot (one time) =="
    chroot "$BR" sh -c 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        --no-install-recommends gcc-arm-linux-gnueabi libc6-dev-armel-cross \
        binutils-arm-linux-gnueabi gcc g++ make autoconf automake libtool pkg-config \
        patchelf git ca-certificates python3 file patch'
fi

chroot "$BR" env RB_IN_BUILD_CHROOT=1 TARBALL_NAME="$TARBALL_NAME" HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    sh /src/scripts/odroid/build-on-odroid.sh "$STEP"

echo
echo "built: $REPO/work/$TARBALL_NAME"
echo "next:  sh $REPO/scripts/device/install.sh $REPO/work/$TARBALL_NAME"
