#!/usr/bin/env bash
# fleet_check.sh - the security + health posture of the swarm, one glance.
# Run from anywhere: tools/fleet_check.sh
# Every line must read PASS before the router forwards anything to the mask.
set -u
# Addresses are DISCOVERED, never assumed: follower IPs are DHCP leases and
# move, and a hardcoded list reports a healthy node as dead.
# Override for speed once discovered:  NODES="N1 192.0.2.10|N2 192.0.2.11" ...
if [ -n "${NODES:-}" ]; then
  IFS='|' read -ra NODES <<< "$NODES"
else
  # Ask the fleet where it lives. Every node gossips its DHCP address in the
  # heartbeat, so any reachable blade can name all the others - the admin port
  # (8443, never forwarded) serves that view. Sweeping the subnet is the last
  # resort for when nothing answers at all.
  SEED="${SEED:-$(grep BOHUN_MASK_IP "$(dirname "$0")/../firmware/eth/main/secrets.h" 2>/dev/null | cut -d'"' -f2)}"
  mapfile -t NODES < <(
    curl -k -s --max-time 5 "https://$SEED:8443/roster" 2>/dev/null | python3 -c "
import json,sys
try:
    r = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for m in sorted(r['members'], key=lambda x: x['id']):
    ip = m.get('ip', '0.0.0.0')
    if m.get('eligible') and ip not in ('0.0.0.0', None):
        print(m['name'], ip)
" 2>/dev/null || "$(dirname "$0")/fleet_discover.sh" 2>/dev/null | sort | awk '{print $1" "$2}'
  )
fi
if [ "${#NODES[@]}" -eq 0 ]; then
  echo "no blades answered on the subnet - is the switch up?"; exit 1
fi
# Deliberately WRONG. A posture check must never present a valid key: a door
# that accepted it would erase the staging slot before validating the body.
# All this needs to know is whether /ota exists on 443 (403 = there, refusing)
# or not (404 = the split holds).
KEY="posture-probe-not-a-real-key"
fail=0
builds=""

# PER-NODE IDENTITY. Port 443 on a leading blade is the splicer, which relays
# to whichever backend it picks - probing it for a node's own identity reads
# somebody else's build stamp. Per-node checks go to the blade's own backend
# port; 443 is still checked as the public path. The fallback covers a build
# that serves 443 directly with no backend port.
BACKEND_PORT=8080
own_page() {   # $1 = ip -> headers from THIS blade, not whoever it proxies to
  curl -sk -o /dev/null -D - --max-time 6 "https://$1:$BACKEND_PORT/" 2>/dev/null ||
  curl -sk -o /dev/null -D - --max-time 6 "https://$1/" 2>/dev/null || true
}

printf "%-4s %-16s %-10s %-14s %-12s %-16s %s\n" NODE IP PAGE "/ota@443" "8443(admin)" CONTENT VERDICT
for entry in "${NODES[@]}"; do
  set -- $entry; name=$1; ip=$2
  hdrs=$(own_page "$ip")
  page=$(echo "$hdrs" | head -1 | awk '{print $2}'); page=${page:---}
  build=$(echo "$hdrs" | grep -i x-bohun-page | tr -d '\r' | awk '{print $2}'); build=${build:-?}
  ota443=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 6 -X POST \
           -H "X-Bohun-Key: $KEY" --data "x" "https://$ip/ota" || echo "---")
  # the public path itself: 443 must answer 200 whether it is a splicer or a
  # direct server. This is the check a visitor's browser performs.
  pub=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 10 "https://$ip/" || echo "---")
  # 404 = no /ota on the public port (what we want). 403 = door present but refusing
  # our bogus key, which still means /ota is exposed on 443 - a FAIL either way.
  if nc -z -G 3 "$ip" 8443 2>/dev/null; then admin="OPEN"; else admin="CLOSED"; fi
  verdict="PASS"
  [ "$page" = "200" ] || verdict="FAIL(own page on $BACKEND_PORT)"
  [ "$pub" = "200" ] || verdict="FAIL(public 443: $pub)"
  [ "$ota443" = "404" ] || verdict="FAIL(/ota answers on 443: $ota443)"
  [ "$admin" = "OPEN" ] || verdict="FAIL(no admin port = no OTA path)"
  [ "$verdict" = "PASS" ] || fail=1
  printf "%-4s %-16s %-10s %-14s %-12s %-16s %s\n" "$name" "$ip" "$page" "$ota443" "$admin" "$build" "$verdict"
  builds="$builds|$build"
done
n_builds=$(echo "$builds" | tr '|' '\n' | grep -v '^$' | sort -u | wc -l | tr -d ' ')
[ "$n_builds" -le 1 ] || { echo "WARN: nodes serve $n_builds different page versions - a deploy was missed"; fail=1; }

echo
echo "roster from the mask holder:"
curl -sk --max-time 5 "https://$SEED:8443/roster" | python3 -c "
import json,sys
try:
    r = json.load(sys.stdin)
    print('  leader:', r['leader'])
    for m in sorted(r['members'], key=lambda x: x['id']):
        print(f\"    {m['name']:22} {m['state']:8} up={m['uptime_s']:>6}s\")
    sp = [m for m in r['members'] if m.get('self')][0].get('splice')
    if sp:
        print(f\"  splice: {sp['backends']} backends, {sp['busy']} busy, \"
              f\"{sp['accepted']} accepted / {sp['done']} done / {sp['refused']} refused\")
except Exception as e:
    print('  roster unreadable:', e)
"
# ── staleness guards ────────────────────────────────────────────────────────
# Two ways the page silently goes stale:
#   1. assets/personal.html edited but the gz/br blobs never regenerated
#   2. blobs regenerated but the firmware never rebuilt/deployed
# Both are caught by CONTENT, never mtime. The served X-Bohun-Page tag is
# size-crc32 of the embedded blobs, so the stored blobs' tag answers 2.
HERE="$(dirname "$0")"
SRC="$HERE/../assets/personal.html"
GZ="$HERE/../firmware/eth/main/personal.html.gz"
BR="$HERE/../firmware/eth/main/personal.html.br"
echo
if [ -f "$SRC" ] && [ -f "$GZ" ] && [ -f "$BR" ]; then
  eval "$(python3 - "$SRC" "$GZ" "$BR" <<'PY'
import gzip, sys, zlib
src, gz, br = sys.argv[1], sys.argv[2], sys.argv[3]
stored = open(gz, 'rb').read()
stored_br = open(br, 'rb').read()
# Compare DECOMPRESSED content, not the container: GNU gzip and python's gzip
# write a different OS byte in the header, so the raw bytes never match even
# when the payload is identical. What we actually care about is the payload.
same = gzip.decompress(stored) == open(src, 'rb').read()
print(f"BLOB_CURRENT={'yes' if same else 'no'}")
# X-Bohun-Page covers BOTH encodings - a gz-only refresh must not read as
# consistent while brotli clients get stale bytes:
# gzlen-gzcrc32-brlen-brcrc32, matching page_tag() in serve.c.
print(f"STORED_TAG={len(stored)}-{zlib.crc32(stored) & 0xffffffff:08x}-"
      f"{len(stored_br)}-{zlib.crc32(stored_br) & 0xffffffff:08x}")
PY
)"
  if [ "$BLOB_CURRENT" != "yes" ]; then
    echo "WARN: personal.html.gz is STALE vs assets/personal.html - regenerate:"
    echo "      gzip -9 -n -c assets/personal.html > firmware/eth/main/personal.html.gz"
    echo "      brotli -f -q 11 assets/personal.html -o firmware/eth/main/personal.html.br"
    fail=1
  fi
  served=$(echo "$builds" | tr '|' '\n' | grep -v '^$' | sort -u | head -1)
  if [ -n "$served" ] && [ "$served" != "$STORED_TAG" ]; then
    echo "WARN: fleet serves [$served] but the local blob is [$STORED_TAG] - deploy is behind"
    fail=1
  fi
fi

exit $fail
