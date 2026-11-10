#!/bin/zsh
# mkzip.sh - Pica release packaging on the Mac (T18). Run after, in the VM:
#   w98 build editor release            -> release\Pica-<ver>\ (pica.exe + README.TXT)
#   w98 build 'editor\setup' clean      -> (clean first: nmake once kept a stale setup.res, docs/INSTALLER.md)
#   w98 build 'editor\setup'            -> setup\setup.exe with the final pica.exe and README.TXT inside
# Then: tools/mkzip.sh   (from anywhere)
# Makes release/pica-<ver>.zip (folder Pica\ inside: unzip to C:\ gives C:\PICA) from an explicit file list, so
# settings, session, backups or logs that a test run left in the release folder can never ship, and copies
# setup/setup.exe to release/pica-<ver>-setup.exe after checking that it embeds exactly this pica.exe and README.TXT.
set -e
ROOT="${0:A:h:h}"
VER=$(sed -n 's/^#define APP_VERSION *"\([^"]*\)".*/\1/p' "$ROOT/res/appinfo.h")
[ -n "$VER" ] || { echo "mkzip: APP_VERSION not found in res/appinfo.h" >&2; exit 1; }
REL="$ROOT/release/Pica-$VER"
ZIP="$ROOT/release/pica-$VER.zip"
SETUP_OUT="$ROOT/release/pica-$VER-setup.exe"
FILES=(pica.exe README.TXT)

for f in $FILES; do
  [ -f "$REL/$f" ] || { echo "mkzip: $REL/$f missing (run: w98 build editor release)" >&2; exit 1; }
done
cmp -s "$ROOT/README.TXT" "$REL/README.TXT" || { echo "mkzip: release README.TXT differs from ./README.TXT (rerun release)" >&2; exit 1; }
python3 - "$REL/README.TXT" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
bad = [i for i, c in enumerate(b) if c > 126 or (c < 32 and c not in (9, 10, 13))]
lf = b.count(b'\n'); crlf = b.count(b'\r\n')
if bad or lf != crlf:
    sys.exit('mkzip: README.TXT must be plain ASCII with CRLF line endings (bad bytes %d, LF %d, CRLF %d)' % (len(bad), lf, crlf))
EOF
[ -z "$(ls -A "$REL" | grep -v -x -e pica.exe -e README.TXT)" ] || echo "mkzip: note: extra files in $REL are NOT zipped: $(ls -A "$REL" | grep -v -x -e pica.exe -e README.TXT | tr '\n' ' ')"

STAGE=$(mktemp -d)
mkdir "$STAGE/Pica"
for f in $FILES; do cp -p "$REL/$f" "$STAGE/Pica/$f"; done
rm -f "$ZIP"
(cd "$STAGE" && zip -qX "$ZIP" Pica/pica.exe Pica/README.TXT)
rm -rf "$STAGE"
echo "zip:   ${ZIP#$ROOT/}"
unzip -l "$ZIP" | sed 's/^/       /'

if [ -f "$ROOT/setup/setup.exe" ]; then
  python3 - "$ROOT/setup/setup.exe" "$REL/pica.exe" "$REL/README.TXT" <<'EOF'
import sys
setup = open(sys.argv[1], 'rb').read()
for p in sys.argv[2:]:
    if setup.find(open(p, 'rb').read()) < 0:
        sys.exit('mkzip: setup/setup.exe does not embed this %s: rebuild it (w98 build editor\\setup clean, then w98 build editor\\setup)' % p.split('/')[-1])
EOF
  cp -p "$ROOT/setup/setup.exe" "$SETUP_OUT"
  echo "setup: ${SETUP_OUT#$ROOT/} (embeds this pica.exe and README.TXT)"
else
  echo "mkzip: setup/setup.exe not built; no ${SETUP_OUT#$ROOT/}" >&2
  exit 1
fi
echo "sizes and MD5:"
for f in "$REL/pica.exe" "$REL/README.TXT" "$ZIP" "$SETUP_OUT"; do
  printf '       %9d  %s  %s\n' "$(stat -f %z "$f")" "$(md5 -q "$f")" "${f#$ROOT/}"
done
