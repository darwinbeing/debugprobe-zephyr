#!/usr/bin/env bash
# SWD throughput A/B benchmark — measures pure DAP_TransferBlock read throughput.
#
# Method: isolate transfer time from fixed connect overhead by subtraction.
#   T_overhead = time(init; halt; shutdown)                 -- connect + halt only
#   T_total    = time(init; halt; dump <N> bytes; shutdown) -- connect + halt + read
#   transfer   = T_total - T_overhead   ->  throughput = N / transfer
# Non-destructive: reads RAM only (no flash erase/program).
#
# Usage: swd_throughput_bench.sh <label> <out_file>
set -u

LABEL="${1:?need label (e.g. ours|official)}"
OUT="${2:?need output file}"

IFACE="interface/cmsis-dap.cfg"
TARGET="target/rp2040.cfg"
ADDR="0x20000000"          # RP2040 SRAM base
SIZE=262144               # 256 KiB
SPEEDS=(1000 5000 15000)  # adapter speed kHz
REPS=3
TMP="$(mktemp -d)"

# Run openocd headless with the given -c body; print elapsed real seconds.
run_timed() {  # $1 = speed kHz, $2 = body
  local spd="$1" body="$2" log="$TMP/oocd.log" t0 t1 rc
  t0=$(python3 -c 'import time;print(time.time())')
  openocd -f "$IFACE" -c "adapter speed $spd" -f "$TARGET" \
    -c "init" -c "$body" -c "shutdown" >"$log" 2>&1
  rc=$?
  t1=$(python3 -c 'import time;print(time.time())')
  if [ "$rc" -ne 0 ] || grep -qiE "error|fail" "$log"; then
    echo "ERR"
    return 1
  fi
  python3 -c "print(f'{$t1-$t0:.3f}')"
}

min() { sort -n | head -1; }

{
  echo "# label=$LABEL  $(date)"
  echo "# read $SIZE bytes from $ADDR; transfer = total - overhead; min of $REPS reps"
  echo "# speed_kHz  overhead_s  total_s  transfer_s  KBps"
} | tee -a "$OUT"

for spd in "${SPEEDS[@]}"; do
  ov=(); tot=()
  for ((i=0;i<REPS;i++)); do
    o=$(run_timed "$spd" "halt")                                   || { echo "$spd  CONNECT_FAIL" | tee -a "$OUT"; continue 2; }
    t=$(run_timed "$spd" "halt; dump_image $TMP/d.bin $ADDR $SIZE") || { echo "$spd  READ_FAIL"    | tee -a "$OUT"; continue 2; }
    ov+=("$o"); tot+=("$t")
  done
  mo=$(printf '%s\n' "${ov[@]}"  | min)
  mt=$(printf '%s\n' "${tot[@]}" | min)
  awk -v s="$spd" -v o="$mo" -v t="$mt" -v n="$SIZE" \
    'BEGIN{ tr=t-o; if(tr<=0)tr=0.0001; printf "%-10s  %8.2f  %7.2f  %9.2f  %7.1f\n", s, o, t, tr, (n/1024)/tr }' \
    | tee -a "$OUT"
done

rm -rf "$TMP"
