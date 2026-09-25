#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT
mkdir -p "$TEMP/bin"
printf '#!/bin/sh\necho ORIGINAL\n' > "$TEMP/bin/benzware"
printf '#!/bin/sh\necho DUMPER\n' > "$TEMP/bin/benzhack_dump"
chmod +x "$TEMP/bin/benzware" "$TEMP/bin/benzhack_dump"

# Extract the real launcher template from build.sh rather than maintaining a copy.
awk '/^cat > .*benzware.sh.*SH_EOF/ {copy=1; next} copy && /^SH_EOF$/ {exit} copy {print}' \
    "$ROOT/build.sh" > "$TEMP/template"
[ -s "$TEMP/template" ] || { echo 'launcher template missing' >&2; exit 1; }
benz_hash=$(sha256sum "$TEMP/bin/benzware" | cut -d ' ' -f 1)
dump_hash=$(sha256sum "$TEMP/bin/benzhack_dump" | cut -d ' ' -f 1)
sed "s/__BENZWARE_SHA256__/$benz_hash/; s/__DUMPER_SHA256__/$dump_hash/" \
    "$TEMP/template" > "$TEMP/launcher"
tar -czf "$TEMP/payload" -C "$TEMP/bin" benzware benzhack_dump
cat "$TEMP/payload" >> "$TEMP/launcher"
chmod +x "$TEMP/launcher"
[ "$(TMPDIR="$TEMP" "$TEMP/launcher")" = ORIGINAL ]
[ "$(TMPDIR="$TEMP" "$TEMP/launcher" dump)" = DUMPER ]

# Replace just the compressed payload but not the hash in the wrapper.
printf '#!/bin/sh\necho TAMPERED\n' > "$TEMP/bin/benzware"
tar -czf "$TEMP/payload-bad" -C "$TEMP/bin" benzware benzhack_dump
awk '/^__PAYLOAD_BELOW__$/ {print; exit} {print}' "$TEMP/launcher" > "$TEMP/tampered"
cat "$TEMP/payload-bad" >> "$TEMP/tampered"
chmod +x "$TEMP/tampered"
if TMPDIR="$TEMP" "$TEMP/tampered" > "$TEMP/out" 2> "$TEMP/error"; then
    echo 'Tampered payload unexpectedly ran' >&2
    exit 1
fi
grep -q 'checksum mismatch' "$TEMP/error"
[ ! -s "$TEMP/out" ]
echo 'Launcher integrity tests: OK'
