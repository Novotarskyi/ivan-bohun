#!/usr/bin/env bash
# deploy.sh - push the eth image to every serving blade, leader LAST, and PROVE
# each one took it before touching the next.
#
# The OTA key comes from the environment, never from this file - the script is
# safe to publish. Get it from main/secrets.h:
#   export BOHUN_OTA_KEY=$(grep BOHUN_OTA_KEY firmware/eth/main/secrets.h | cut -d'"' -f2)
#
# EVERY PUSH IS VERIFIED. A push loop that discards curl's result can silently
# no-op the whole fleet while the firmware answers honestly (403 bad key,
# 409 wrong variant, 400 short image, or "ok, rebooting") - so every reply is
# read, and three gates must pass per node before the next node is touched:
#   1. TRANSFER  - curl exits 0, HTTP 200, body starts "ok, rebooting"
#   2. REBOOT    - uptime_s comes back LOWER than it was before the push
#   3. NEW BUILD - X-Bohun-Build differs from what it served before
# Any failure aborts. Leader-last ordering means an abort leaves the public face
# still serving the last good image.
#
# GATE 4, THE TRIAL. A fresh image is on TRIAL for 90 s of continuous serving
# (main/selftest.c SOAK_S) with a 300 s deadline, after which the node reboots
# and the bootloader restores the previous slot - so a node can pass the first
# three gates and still roll back minutes later. Every node publishes
# `on_trial` on the admin roster; this polls for the real answer and only
# calls it a release when every node has earned permanence.
set -u
: "${BOHUN_OTA_KEY:?set BOHUN_OTA_KEY first - see the comment above}"

HERE="$(cd "$(dirname "$0")/.." && pwd)"          # repo root, whatever the cwd
BIN="${BIN:-$HERE/firmware/eth/build.lilygo/bohun_eth.bin}"
SEED="${SEED:-$(grep BOHUN_MASK_IP "$HERE/firmware/eth/main/secrets.h" 2>/dev/null | cut -d'"' -f2)}"
[ -f "$BIN" ] || { echo "no image at $BIN"; exit 1; }

# WHAT IS BEING PUSHED, by identity rather than by "different from before".
# The ELF SHA is computed at link time over the whole image, so it changes when
# anything does - unlike __DATE__/__TIME__, which only move when the file that
# prints them recompiles and can call a changed image unchanged. Reading it
# from the app descriptor also makes the run IDEMPOTENT: a node already
# carrying this exact image is skipped.
TARGET_SHA=$(python3 - "$BIN" <<'PYEOF'
import sys
b = open(sys.argv[1], "rb").read()
print(b[0x20:0x20+256][144:148].hex())     # esp_app_desc_t.app_elf_sha256[0:4]
PYEOF
)
[ -n "$TARGET_SHA" ] || { echo "cannot read the image's ELF SHA - is $BIN a real app image?"; exit 1; }
echo "pushing build $TARGET_SHA"

# --- who gets it, and in what order -----------------------------------------
# state == alive matters: an eligible-but-LOST node is pushed to a stale address
# (the addresses rotate - see fleet_discover.sh), which burns the full timeout
# and then fails. Better skipped, and announced rather than discovered.
mapfile -t NODES < <(curl -k -s --max-time 5 "https://$SEED:8443/roster" 2>/dev/null | python3 -c "
import json,sys
r = json.load(sys.stdin)
lead = r['leader']
alive = [m for m in r['members']
         if m.get('eligible') and m.get('state') == 'alive'
         and m.get('ip', '0.0.0.0') != '0.0.0.0']
names = {m['name'] for m in alive}
for m in r['members']:
    if m.get('eligible') and m['name'] not in names:
        print('#SKIP', m['name'])
alive.sort(key=lambda m: (m['name'] == lead, m['id']))   # leader last
for m in alive:
    print(m['name'], m['ip'])
")
[ "${#NODES[@]}" -gt 0 ] || { echo "fleet did not answer on $SEED:8443"; exit 1; }

probe_uptime() {   # $1 = ip -> this node's own uptime_s, or -1 if it will not say
  curl -k -s --max-time 8 "https://$1:8443/roster" 2>/dev/null | python3 -c "
import json,sys
try:
    d = json.load(sys.stdin)
    print([m for m in d['members'] if m['self']][0].get('uptime_s', -1))
except Exception:
    print(-1)"
}
probe_trial() {    # $1 = ip -> "true" while this image is still on trial
  curl -k -s --max-time 8 "https://$1:8443/roster" 2>/dev/null | python3 -c "
import json,sys
try:
    d = json.load(sys.stdin)
    print(str([m for m in d['members'] if m['self']][0].get('on_trial', False)).lower())
except Exception:
    print('unknown')"
}
probe_build() {    # $1 = ip -> the X-Bohun-Build stamp THIS blade serves
  # Port 443 on a leader is the SPLICER: asking it for a build stamp returns
  # whichever backend it picked. The backend port is the blade's OWN server and
  # the only honest answer - deliberately NO fallback to 443, which would read
  # a neighbour's stamp during the reboot window.
  curl -k -s --max-time 8 -o /dev/null -D - "https://$1:8080/" 2>/dev/null |
    tr -d '\r' | awk -F': ' 'tolower($1)=="x-bohun-build"{print $2}' 
}

pushed=0
for entry in "${NODES[@]}"; do
  read -r name ip <<< "$entry"                    # no globbing, no set -u trap
  if [ "$name" = "#SKIP" ]; then
    printf "%-6s %-16s SKIPPED - eligible but not alive in the roster\n" "$ip" ""
    continue
  fi
  printf "%-6s %-16s " "$name" "$ip"

  # GATE 0: never push to a node that is still on trial. ESP-IDF refuses
  # esp_ota_begin() while the running image is PENDING_VERIFY, and the node
  # answers an honest 500 "ota_begin failed" that reads like a firmware
  # fault. The trial is 90 s; wait it out.
  for _ in $(seq 1 40); do
    [ "$(probe_trial "$ip")" != "true" ] && break
    printf "on trial, waiting… "
    sleep 5
  done

  if [ "$(probe_build "$ip" | awk '{print $NF}')" = "$TARGET_SHA" ]; then
    echo "already on $TARGET_SHA - skipped"
    continue
  fi

  was_up=$(probe_uptime "$ip")
  was_build=$(probe_build "$ip")

  # --- gate 1: the transfer itself -------------------------------------------
  body=$(mktemp)
  code=$(curl -k -s --max-time 90 -o "$body" -w '%{http_code}' \
         -H "X-Bohun-Key: $BOHUN_OTA_KEY" \
         --data-binary @"$BIN" "https://$ip:8443/ota" 2>/dev/null) || code="000"
  if [ "$code" != "200" ] || ! grep -q '^ok, rebooting' "$body"; then
    echo "FAILED (http $code): $(head -c 120 "$body" | tr -d '\n')"
    rm -f "$body"
    echo
    echo "ABORTED after $pushed node(s) - the fleet is MIXED. Re-run when fixed."
    echo "Leader was last in the queue, so the public face is on the old image."
    exit 1
  fi
  rm -f "$body"
  printf "sent, "

  # --- gates 2 + 3: it rebooted, and into a different build -------------------
  ok=""
  for _ in $(seq 1 20); do                        # up to ~60 s
    sleep 3
    now_up=$(probe_uptime "$ip")
    [ "$now_up" = "-1" ] && continue              # still down, keep waiting
    if [ "$was_up" = "-1" ] || [ "$now_up" -lt "$was_up" ]; then
      now_build=$(probe_build "$ip")
      if [ "$(printf '%s' "$now_build" | awk '{print $NF}')" = "$TARGET_SHA" ]; then
        ok="yes"
        break
      fi
    fi
  done
  if [ -z "$ok" ]; then
    echo "DID NOT COME BACK ON $TARGET_SHA (was up=${was_up}s build='${was_build}')"
    echo
    echo "ABORTED after $pushed node(s). This node accepted the bytes but did not"
    echo "come back on the new image - read https://$ip:8443/blackbox before"
    echo "pushing anything further."
    exit 1
  fi
  echo "rebooted into the new build"
  pushed=$((pushed + 1))
done

# --- gate 4: every node must EARN permanence, not just survive a sleep -------
echo
echo "$pushed node(s) took the image. Waiting for the trial to mark each one valid"
echo "(90 s of continuous serving each; rollback deadline is 300 s)..."
still=""
for _ in $(seq 1 40); do                          # up to ~200 s
  still=""
  for entry in "${NODES[@]}"; do
    read -r name ip <<< "$entry"
    [ "$name" = "#SKIP" ] && continue
    t=$(probe_trial "$ip")
    [ "$t" = "true" ] && still="$still $name"
    [ "$t" = "unknown" ] && still="$still $name(?)"
  done
  [ -z "$still" ] && break
  printf "\r  still on trial:%s      " "$still"
  sleep 5
done
echo
if [ -n "$still" ]; then
  echo "STILL ON TRIAL after the deadline:$still"
  echo "Those nodes have NOT earned permanence - expect the bootloader to restore"
  echo "the previous image on their next reboot. Read :8443/blackbox on each."
  "$HERE/tools/fleet_check.sh"
  exit 1
fi
echo "  every node marked VALID by its trial - this is a release."
echo
"$HERE/tools/fleet_check.sh"
