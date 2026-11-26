#!/usr/bin/env bash
# Builds the Linux release: dist/OpenJamboree-v<version>-Linux-x64.tar.xz
# Contents: the tracked source (git ls-files), recomplauncher.json at the top and tools/nsp_extract (self-contained
# linux-x64). The player's PC builds the runtime and the game (launcher/prepare.sh). No game files or keys.
#   launcher/make_release.sh 0.1.0
set -euo pipefail
VERSION=${1:?version}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
dotnet publish tools/nsp_extract -c Release -r linux-x64 -o tools/nsp_extract/out-linux -v q
STAGE="dist/OpenJamboree-v$VERSION"
rm -rf "$STAGE"; mkdir -p "$STAGE/tools"
git ls-files -z | xargs -0 -I{} cp --parents {} "$STAGE/"
cp tools/nsp_extract/out-linux/nsp_extract "$STAGE/tools/nsp_extract"
chmod +x "$STAGE/tools/nsp_extract" "$STAGE/launcher/prepare.sh" "$STAGE/tools/build_linux_libs.sh"
if find "$STAGE" -type f \( -name '*.nsp' -o -name '*.nca' -o -name '*.xci' -o -name '*.nso' -o -name '*.nro' -o -name '*.keys' \) | grep -q .; then
  echo "game files in the release" >&2; exit 1; fi
tar -C "$STAGE" -cJf "dist/OpenJamboree-v$VERSION-Linux-x64.tar.xz" .
echo "dist/OpenJamboree-v$VERSION-Linux-x64.tar.xz"
