#!/usr/bin/env bash
# fleet_discover.sh - find the blades by asking them who they are.
#
# Follower addresses come from DHCP and move, so any hardcoded IP eventually
# reports a healthy node as dead. Mapping by MAC does not work either - the
# ledger stores each node's RADIO mac, while ARP sees its ETHERNET mac. So:
# sweep the /24, and for every host that answers TLS, read /roster and take
# the node at its word.
#
# Prints "NAME<TAB>IP" per line, sorted. Cache it: SUBNET=192.0.2 ./fleet_discover.sh
set -u
SUBNET="${SUBNET:-$(grep BOHUN_MASK_IP "$(dirname "$0")/../firmware/eth/main/secrets.h" 2>/dev/null | cut -d'"' -f2 | cut -d. -f1-3)}"

probe() {
  local ip="$1"
  local name
  name=$(curl -k -s --max-time 2 "https://$ip/roster" 2>/dev/null \
         | python3 -c "import json,sys; print(json.load(sys.stdin)['self'])" 2>/dev/null)
  [ -n "${name:-}" ] && printf '%s\t%s\n' "$name" "$ip"
}
export -f probe 2>/dev/null || true

for i in $(seq 1 254); do
  probe "$SUBNET.$i" &
  while [ "$(jobs -rp | wc -l)" -ge 48 ]; do wait -n 2>/dev/null || sleep 0.05; done
done
wait
