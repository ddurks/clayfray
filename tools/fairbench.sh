#!/bin/sh
# Fair A/B under a machine that throttles.
#
# The naive protocol (run A, then run B) is systematically biased: this M2
# drifts 66.6 -> 56.7 -> 24.0 fps across three IDENTICAL back-to-back runs, so
# whichever config runs second is penalised by up to 2.8x. That is larger than
# every effect measured tonight.
#
# So: INTERLEAVE A,B,A,B,... and take the MEDIAN per config. Both configs then
# see the same thermal profile, and the median rejects the outlier a single
# hot pass would otherwise contribute. Cooldown between runs.
#
# usage: fairbench.sh "<labelA>:<envA>" "<labelB>:<envB>" [reps] [cooldown_s]
cd /Users/onlinedavid/code/clayfray || exit 1
A="$1"; B="$2"; REPS="${3:-3}"; COOL="${4:-8}"

one() { # $1 env string -> ms/frame for this pass
  s=$(python3 -c 'import time;print(time.time())')
  env $1 ./build/clayfray --res 640x360 --exit-after 60 >/dev/null 2>&1
  t1=$(python3 -c "import time;print(time.time()-$s)")
  s=$(python3 -c 'import time;print(time.time())')
  env $1 ./build/clayfray --res 640x360 --exit-after 360 >/dev/null 2>&1
  t2=$(python3 -c "import time;print(time.time()-$s)")
  python3 -c "print((($t2)-($t1))/300*1000)"
}

env ${A#*:} ./build/clayfray --res 640x360 --exit-after 30 >/dev/null 2>&1  # warm pipelines
AS=""; BS=""
i=1
while [ "$i" -le "$REPS" ]; do
  sleep "$COOL"; AS="$AS $(one "${A#*:}")"
  sleep "$COOL"; BS="$BS $(one "${B#*:}")"
  i=$((i+1))
done
python3 - "$AS" "$BS" "${A%%:*}" "${B%%:*}" <<'PY'
import sys, statistics as st
a=[float(x) for x in sys.argv[1].split()]
b=[float(x) for x in sys.argv[2].split()]
na, nb = sys.argv[3], sys.argv[4]
for n,v in ((na,a),(nb,b)):
    print(f"  {n:<14} median {st.median(v):6.2f} ms  ({1000/st.median(v):5.1f} fps)   samples " +
          " ".join(f"{x:.1f}" for x in v))
d = (st.median(b)-st.median(a))/st.median(a)*100
print(f"  -> {nb} is {d:+.1f}% vs {na} (median, interleaved)")
PY
