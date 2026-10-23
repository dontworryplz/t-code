#!/bin/sh
set -eu

case "${0##*/}" in
  c++|g++) compiler=g++-15 ;;
  clang|clang++)
    compiler=${0##*/}
    set -- "--gcc-toolchain=$SNAP/toolchain/usr" "$@"
    ;;
  *) compiler=gcc-15 ;;
esac
triplet=$(cat "$SNAP/usr/lib/t-code/toolchain-triplet")
# GCC locates cc1, crtbegin and libgcc relative to this prefix; ld needs libc's crt files too.
export GCC_EXEC_PREFIX="$SNAP/toolchain/usr/lib/gcc/"
exec "$SNAP/toolchain/usr/bin/$compiler" --sysroot="$SNAP/toolchain" \
  -B"$SNAP/toolchain/usr/lib/gcc/$triplet/15/" -B"$SNAP/toolchain/usr/lib/$triplet/" "$@"
