#!/bin/sh
# clayfray ctl client: send commands to a running clayfray (windowed or
# --serve) and print the response. One argument per command line:
#
#   tools/ctl.sh "set look.aoStrength 0.8" "shot lookdev/probe.png"
#   tools/ctl.sh stats
#   tools/ctl.sh "edit carve 0.02 0.42 0.12 0.07" "shot lookdev/wound.png"
#
# Env: CLAYFRAY_CTL=dir (default ./ctl), CLAYFRAY_CTL_TIMEOUT=sec (default 20).
set -u
dir="${CLAYFRAY_CTL:-ctl}"
timeout="${CLAYFRAY_CTL_TIMEOUT:-20}"
[ $# -ge 1 ] || { echo "usage: $0 '<command>' ..." >&2; exit 2; }
mkdir -p "$dir/in" "$dir/out"

name="$(date +%s)_$$"
tmp="$dir/in/.$name.tmp"
: > "$tmp"
for c in "$@"; do printf '%s\n' "$c" >> "$tmp"; done
mv "$tmp" "$dir/in/$name"   # rename = atomic hand-off to the poller

deadline=$(( $(date +%s) + timeout ))
while [ ! -f "$dir/out/$name" ]; do
  if [ "$(date +%s)" -gt "$deadline" ]; then
    echo "ctl: timed out after ${timeout}s (is clayfray running?)" >&2
    rm -f "$dir/in/$name"
    exit 1
  fi
  # sub-second poll; perl is stock on macOS, sleep(1) is the fallback
  perl -e 'select undef, undef, undef, 0.03' 2>/dev/null || sleep 1
done
cat "$dir/out/$name"
rm -f "$dir/out/$name"
