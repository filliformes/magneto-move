#!/bin/bash
set -e
MODULE_ID="magneto"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
cd "$ROOT"                          # run from repo root so a relative build context works
ROOT_WIN="$(pwd -W 2>/dev/null || pwd)"   # Windows-style path for docker cp on MSYS
IMAGE="${MODULE_ID}-builder"

echo "Building $MODULE_ID for ARM64 (aarch64)..."
# Relative "." context avoids MSYS mangling an absolute /c/... path into an invalid path.
MSYS_NO_PATHCONV=1 docker build -t "$IMAGE" -f scripts/Dockerfile .

# Create a container, copy sources in, build, copy the .so out (cross-platform safe).
CID=$(MSYS_NO_PATHCONV=1 docker create -w /build "$IMAGE" bash -c '
    dos2unix /build/src/dsp/*.c 2>/dev/null || true
    mkdir -p /build/dist/magneto
    aarch64-linux-gnu-gcc -O2 -shared -fPIC -ffast-math \
      -o /build/dist/magneto/magneto.so /build/src/dsp/*.c -I/build/src/dsp -lm
    cp /build/src/module.json /build/dist/magneto/
    cp /build/src/help.json /build/dist/magneto/
    ls -la /build/dist/magneto/
')
docker cp "$ROOT_WIN/src" "$CID:/build/src"
docker start -a "$CID"
# set -e does NOT catch docker failures on Git Bash/MSYS — check explicitly or we
# silently deploy the previous build's stale .so.
EXIT_CODE=$(docker inspect "$CID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: compile failed inside container (exit $EXIT_CODE). See output above."
    docker rm "$CID" >/dev/null 2>&1 || true
    exit 1
fi
mkdir -p dist/magneto
docker cp "$CID:/build/dist/magneto/magneto.so" "$ROOT_WIN/dist/magneto/"
docker cp "$CID:/build/dist/magneto/module.json" "$ROOT_WIN/dist/magneto/"
docker cp "$CID:/build/dist/magneto/help.json" "$ROOT_WIN/dist/magneto/"
docker rm "$CID" >/dev/null
echo "Built: dist/magneto/"
