#!/bin/zsh
# Build ultrafine as a native arm64 binary.
#
# Installs to ~/.local/bin/ultrafine. Pass a file or a directory to build
# somewhere else, e.g. when another project vendors this one.
set -e
src="${0:A:h}"
out="${1:-$HOME/.local/bin}"
[[ -d "$out" ]] && out="$out/ultrafine"
mkdir -p "${out:h}"
clang -O2 -arch arm64 -o "$out" "$src/ultrafine.c" -framework IOKit -framework CoreFoundation
codesign --force --sign - "$out"
file "$out"
