#!/bin/bash
# ioload-budget.sh <device> [buf] [lo_us] [hi_us] [base_us]
#
# The longest single IO cycle a device lets a client have: ioload burns base_us
# (default 100) in every cycle and a longer spike once in 16 cycles, and this
# script bisects the spike between lo_us (default 300) and hi_us (default 800)
# for the smallest one Core Audio reports as an overload. Each step runs 4 s.
# The result is Core Audio's budget for the cycle minus what the path through
# the driver took of it, so a larger figure is a cheaper path. Run it three
# times; the "ok at" figures should agree within about 10 us.
#
# <device> is the ID from `ioload list` or part of the name. buf defaults to 64.
# Needs the ioload binary next to this script (build line in ioload.c).
dir=$(cd "$(dirname "$0")" && pwd)
dev=${1:?usage: ioload-budget.sh <device> [buf] [lo_us] [hi_us] [base_us]}
buf=${2:-64}; lo=${3:-300}; hi=${4:-800}; base=${5:-100}
run() {
  "$dir/ioload" run "$dev" --buf "$buf" --secs 4 --burn-us "$base" --spike-us "$1" --spike-every 16 2>&1 | tail -1
}
count() { sed -E 's/.* ([0-9]+) overloads.*/\1/' <<< "$1"; }
line=$(run "$lo"); n=$(count "$line")
if ! [[ $n =~ ^[0-9]+$ ]]; then echo "ioload did not run: $line"; exit 1; fi
if (( n > 0 )); then echo "already $n overloads at $lo us; give a lower lo_us"; echo "$line"; exit 1; fi
line=$(run "$hi"); n=$(count "$line")
if (( n == 0 )); then echo "no overloads at $hi us; give a higher hi_us"; echo "$line"; exit 1; fi
while (( hi - lo > 8 )); do
  mid=$(( (lo + hi) / 2 )); line=$(run "$mid"); n=$(count "$line")
  echo "  spike $mid us: $n overloads"
  if (( n > 0 )); then hi=$mid; else lo=$mid; fi
done
echo "device $dev, $buf frames: ok at $lo us, overloads at $hi us"
