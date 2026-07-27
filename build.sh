#!/bin/zsh
# Build ultrafine as a native arm64 binary and install it to ~/.local/bin.
set -e
src="${0:A:h}"
clang -O2 -arch arm64 -o "$HOME/.local/bin/ultrafine" "$src/ultrafine.c" -framework IOKit -framework CoreFoundation
codesign --force --sign - "$HOME/.local/bin/ultrafine"
file "$HOME/.local/bin/ultrafine"
