#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
# Build script for Limba: ./build.sh --help says it all.
# No -march=native: nothing here depends on the CPU it is built on.
set -euo pipefail

cd "$(dirname "$0")"

usage()
{
    cat <<'EOF'
Limba build script.

  ./build.sh [variant] [action]

  variant: release (default) | debug | asan | ubsan | tsan
  action:  build (default) | test | clean | clean-build | clean-test

Variants
  release  -O2, assertions off. What is shipped, and the only build a
           speed number may come from.
  debug    -O0 -g. Slow but honest under a debugger.
  asan     AddressSanitizer: out-of-bounds reads and writes, use after
           free, leaks at exit. About 2x slower.
  ubsan    UndefinedBehaviorSanitizer: signed overflow, oversized shifts,
           misaligned or null pointers. Stops at the first one.
  tsan     ThreadSanitizer: data races.

  A sanitizer reports at run time, so it has to be the `test` action, or
  a program of the variant run by hand. Their programs carry a -<variant>
  suffix (bin/<cpu>-<os>/test_ir-asan), so they never overwrite the
  release build, and each variant keeps its own objects.

Actions
  build        compile the library, the tests and the programs
  test         build, then run every test_* of the variant from the root
               of the repository (they read tests/, write nothing); exit 1
               if one fails
  clean        remove the objects of the variant and the programs it made,
               and nothing else
  clean-build  the two in a row, for a build from nothing
  clean-test   the same, then the tests: the honest answer before a commit

Output
  Objects and the static library go to lib/<cpu>-<os>-<variant>/,
  programs to bin/<cpu>-<os>/, with a -<variant> suffix for every variant
  but release. Each src/<tool>/main.c becomes the program <tool> (limba),
  with the other .c files of its directory; every other .c under src/ goes
  into liblimba.a. Each tests/*.c and tools/*.c becomes a program too.

Environment
  CC            the compiler, gcc by default
  AR            the archiver, ar by default
  LIMBA_CFLAGS  flags added to the variant, to try a compiler out

Before a commit: ./build.sh release test and at least asan.

Examples
  ./build.sh                 a release build
  ./build.sh release test    build it and run the tests
  ./build.sh asan test       the same under AddressSanitizer
  ./build.sh asan clean      throw the asan objects away
EOF
}

case "${1:-}" in
-h | --help | help)
    usage
    exit 0
    ;;
esac

VARIANT="${1:-release}"
ACTION="${2:-build}"
CC="${CC:-gcc}"

CPU="$(uname -m)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
OBJ="lib/$CPU-$OS-$VARIANT"
BIN="bin/$CPU-$OS"
SUFFIX=""
[ "$VARIANT" != release ] && SUFFIX="-$VARIANT"

CFLAGS=(-std=gnu11 -Wall -Wextra -Werror -pthread -Isrc -Iinclude)
case "$VARIANT" in
release) CFLAGS+=(-O2 -DNDEBUG) ;;
debug) CFLAGS+=(-O0 -g) ;;
asan) CFLAGS+=(-O1 -g -fsanitize=address -fno-omit-frame-pointer) ;;
ubsan) CFLAGS+=(-O1 -g -fsanitize=undefined -fno-sanitize-recover=all) ;;
tsan) CFLAGS+=(-O1 -g -fsanitize=thread) ;;
*)
    echo "build.sh: unknown variant: $VARIANT" >&2
    case "$VARIANT" in
    build | test | clean)
        echo "build.sh: the variant comes first: ./build.sh release" \
            "$VARIANT" >&2 ;;
    esac
    echo "build.sh: --help says which variants there are" >&2
    exit 2
    ;;
esac

case "$ACTION" in
build | test | clean | clean-build | clean-test) ;;
*)
    echo "build.sh: unknown action: $ACTION" >&2
    echo "build.sh: --help says which actions there are" >&2
    exit 2
    ;;
esac
LDLIBS=(-lm)
if [ -n "${LIMBA_CFLAGS:-}" ]; then
    # shellcheck disable=SC2206
    CFLAGS+=($LIMBA_CFLAGS)
    LDLIBS+=($LIMBA_CFLAGS)
fi
AR="${AR:-ar}"
VERSION="$(git describe --always --dirty 2>/dev/null || echo unknown)"
CFLAGS+=(-DLIMBA_VERSION="\"$VERSION\"")

# Throw away what this variant made, and only what this variant made.
clean()
{
    rm -rf "$OBJ"
    if [ -n "$SUFFIX" ]; then
        rm -f "$BIN"/*"$SUFFIX"
        return
    fi
    for f in "$BIN"/*; do
        [ -e "$f" ] || continue
        case "$f" in
        *-debug | *-asan | *-ubsan | *-tsan) continue ;;
        esac
        rm -f "$f"
    done
}

case "$ACTION" in
clean)
    clean
    exit 0
    ;;
clean-build)
    clean
    ACTION=build
    ;;
clean-test)
    clean
    ACTION=test
    ;;
esac

mkdir -p "$OBJ" "$BIN"

# one object, rebuilt when its source or any header or table is newer
headers_newest=$(find src include \( -name '*.h' -o -name '*.def' \) \
    -printf '%T@\n' | sort -n | tail -1)
compile()
{ # source, object, then the flags
    local src="$1" obj="$2"
    shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] ||
        [ "$(stat -c %Y "$obj")" -lt "${headers_newest%.*}" ]; then
        echo "CC  $src"
        "$CC" "$@" -c "$src" -o "$obj"
    fi
}
objname() { echo "$OBJ/$(echo "${1#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }

# A directory with a main.c is a program: its other .c files are its own.
# Every other .c under src/ goes into the library.
tool_dirs=$(for m in src/*/main.c; do [ -f "$m" ] && dirname "$m"; done)
objs=()
while IFS= read -r src; do
    grep -qx "$(dirname "$src")" <<<"$tool_dirs" && continue
    obj=$(objname "$src")
    objs+=("$obj")
    compile "$src" "$obj" "${CFLAGS[@]}"
done < <(find src -name '*.c' ! -name main.c ! -path 'src/third_party/*' | sort)
rm -f "$OBJ/liblimba.a"
"$AR" rcs "$OBJ/liblimba.a" "${objs[@]}"

# tests and tools, then the programs
for src in tests/*.c tools/*.c src/*/main.c; do
    [ -f "$src" ] || continue
    exe="$BIN/$(basename "${src%.c}")$SUFFIX"
    own=()
    if [ "$(basename "$src")" = main.c ]; then
        dir=$(dirname "$src")
        exe="$BIN/$(basename "$dir")$SUFFIX"
        while IFS= read -r c; do
            obj=$(objname "$c")
            own+=("$obj")
            compile "$c" "$obj" "${CFLAGS[@]}"
        done < <(find "$dir" -maxdepth 1 -name '*.c' ! -name main.c | sort)
    fi
    echo "LD  $exe"
    "$CC" "${CFLAGS[@]}" "$src" "${own[@]}" "$OBJ/liblimba.a" \
        "${LDLIBS[@]}" -o "$exe"
done

if [ "$ACTION" = test ]; then
    status=0
    for t in "$BIN"/test_*"$SUFFIX"; do
        [ -e "$t" ] || continue
        # in release, skip the sanitizer builds (test_x-asan and the like)
        [ -n "$SUFFIX" ] || [[ "$(basename "$t")" != *-* ]] || continue
        echo "RUN $t"
        "$t" || status=1
    done
    exit $status
fi
