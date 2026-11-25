#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
port=${PORT:-5000}
dir=$(mktemp -d "${TMPDIR:-/tmp}/akos4-demo.XXXXXX")
jobs_to_stop=()
cleanup() {
    for pid in "${jobs_to_stop[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM
echo "Журналы этого запуска: $dir"
bin/server 127.0.0.1 "$port" configs/demo.conf "$dir/events.log" > "$dir/server.log" &
server=$!
jobs_to_stop+=("$server")
for ((i=0; i<200; i++)); do
    grep -q '^LISTEN ' "$dir/server.log" && break
    kill -0 "$server" 2>/dev/null || exit 1
    sleep .02
done
grep -q '^LISTEN ' "$dir/server.log"
touch "$dir/monitor.log"
bin/monitor 127.0.0.1 "$port" > >(tee "$dir/monitor.log") &
monitor=$!
jobs_to_stop+=("$monitor")
for ((i=0; i<200; i++)); do
    grep -q '^SNAP_END' "$dir/monitor.log" && break
    sleep .02
done
grep -q '^SNAP_END' "$dir/monitor.log"
for actor in reader:1 reader:2 writer:1; do
    bin/"${actor%:*}" 127.0.0.1 "$port" "${actor#*:}" > "$dir/$actor.log" &
    jobs_to_stop+=("$!")
done
for pid in "${jobs_to_stop[@]}"; do wait "$pid"; done
python3 tests/audit.py "$dir/events.log"
