#!/bin/sh
# One test pass of the Linux game for a desktop or a Steam Deck (Desktop Mode, in a terminal), written to one file.
#
#     tools/linux_test_run.sh <game folder> [seconds per run, default 180]
#
# It runs, inside the game folder: the memory test, the selftest, then the game twice for the given time (shadow storage,
# then driver memory: REKARINA_LINUX_GUEST_MEMORY), and keeps the frame-time summary, the memory lines and any fatal
# line of each run. It ends with the game's problem report. The result is linux-test-<date>.txt in the game folder (and
# the report zip in reports/): send both. The game opens a window and is stopped by this script; leave the controller
# alone and do not use the GPU for anything else meanwhile.
set -u

dir=${1:-}
secs=${2:-180}
[ -x "$dir/Rekarina" ] || { sed -n 2,11p "$0" | sed 's/^# \{0,1\}//'; exit 2; }
cd "$dir"
out="linux-test-$(date +%Y%m%d-%H%M).txt"
say() { printf '%s\n' "$*" | tee -a "$out"; }

: > "$out"
say "== system"
say "date: $(date -Is)"
say "kernel: $(uname -r)"
say "os: $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
say "cpu: $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p')"
say "memory: $(free -m 2>/dev/null | awk '/^Mem:/ {print $2 " MiB"}')"
say "session: ${XDG_SESSION_TYPE:-unknown}, display ${WAYLAND_DISPLAY:-${DISPLAY:-none}}"
if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName|driverInfo|apiVersion" | sed 's/^ */gpu: /' | tee -a "$out"
fi
say "game: $(./Rekarina --help 2>&1 | grep -m1 'version:' | sed 's/^\[host\] //')"

say ""
say "== memory test"
./Rekarina --memory-test 2>&1 | grep -E "RESULT|USABLE|placed:|fd:|import:" | tee -a "$out"

say ""
say "== selftest"
./Rekarina --selftest 2>&1 | grep -E "FAIL|selftest:" | tee -a "$out"

run() {   # run <label> [ENV=VALUE ...]
    label=$1; shift
    say ""
    say "== run: $label ($secs s)"
    rm -f "run-$label.log"
    start=$(date +%s)
    env "$@" timeout -s TERM "$secs" ./Rekarina > "run-$label.log" 2>&1
    say "ended after $(( $(date +%s) - start )) s, exit code $?  (124 is this script's stop, not a crash)"
    grep -E "guest memory:|driver memory at frame|shadow at frame" "run-$label.log" | cut -c1-240 | tee -a "$out"
    grep -E "frametime: exit" "run-$label.log" | tail -1 | cut -c1-300 | tee -a "$out"
    grep -E "frametime:   worst" "run-$label.log" | tail -2 | cut -c1-300 | tee -a "$out"
    grep -E "footprint after the entry step|process: working set" "run-$label.log" | tail -2 | cut -c1-200 | tee -a "$out"
    grep -i -E "fatal|access violation|device lost|crash" "run-$label.log" | head -5 | cut -c1-240 | tee -a "$out"
}
run shadow REKARINA_LINUX_GUEST_MEMORY=shadow
run driver REKARINA_LINUX_GUEST_MEMORY=driver

say ""
say "== report"
./Rekarina --make-report 2>&1 | tail -2 | tee -a "$out"
say ""
say "done: send $dir/$out and the zip in $dir/reports/"
