#!/usr/bin/env bash
set -e

# Upload the flash binaries from build/ to a GitHub release on origin.
# Usage: ./release.sh [tag]   (defaults to the tag on HEAD)

cd "$(dirname "$0")"
TAG="${1:-$(git describe --tags --exact-match)}"
REPO="$(git remote get-url origin)"
cd build

# esp_app_desc_t.version sits right after the image and segment headers
VERSION=$(dd if=esp32-xbee.bin bs=1 skip=48 count=32 status=none | tr -d '\0')
if [ "$VERSION" != "$TAG" ]; then
  echo "esp32-xbee.bin reports version '$VERSION', expected '$TAG'" >&2
  echo "check out $TAG with a clean tree and run 'idf.py reconfigure build'" >&2
  exit 1
fi

# Same files idf.py flash writes
FILES=$(tail -n +2 flash_args | awk '{print $2}')

if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" $FILES -R "$REPO" --clobber
else
  gh release create "$TAG" $FILES -R "$REPO" --verify-tag --draft --generate-notes
  echo "Created draft release $TAG, review and publish it on GitHub"
fi
