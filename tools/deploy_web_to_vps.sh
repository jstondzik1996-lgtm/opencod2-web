#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────────────────
# Canonical web-deploy script.  The _vps_deploy_gsccmp*.sh scripts are LEGACY
# one-shot snapshots and should NOT be used for new deploys.  This script
# deploys to both $DST (redirect) and $DST/r3 (live client) and writes
# remote-config.js for the FS loader.
# ────────────────────────────────────────────────────────────────────────────
# Usage (from repo root, after build-web-wsl succeeds):
#   bash tools/deploy_web_to_vps.sh root@YOUR_HOST [build-dir] [version-tag]
set -eu
set -o pipefail
HOST="${1:?usage: $0 user@host}"
HOSTONLY="${HOST#*@}"
SRC="${2:-build-web-wsl}"
VER="${3:-o1-vidhz-$(date +%Y%m%d%H%M)}"
DST="/var/www/cod2-web"

test -f "$SRC/cod2.html" || { echo "missing $SRC/cod2.html — build first"; exit 1; }
test -f "$SRC/cod2.js"   || { echo "missing $SRC/cod2.js"; exit 1; }
test -f "$SRC/cod2.wasm" || { echo "missing $SRC/cod2.wasm"; exit 1; }

FS_JS="$SRC/cod2_fs.js"
if [[ -f src/web/cod2_fs.js ]]; then FS_JS="src/web/cod2_fs.js"; fi

ssh "$HOST" "mkdir -p /tmp/cod2-web-upload /tmp/cod2-game-relay-upload"
scp "$SRC/cod2.html" "$SRC/cod2.js" "$SRC/cod2.wasm" "$FS_JS" \
    src/web/lobby.js src/web/brand-mark.png src/web/favicon.png src/web/apple-touch-icon.png \
    "$HOST:/tmp/cod2-web-upload/"
if [[ -f "$SRC/cod2.wasm.map" ]]; then
  scp "$SRC/cod2.wasm.map" "$HOST:/tmp/cod2-web-upload/" || true
fi
# Game WS↔UDP relay (Join/connect). Safe to re-copy every deploy.
if [[ -f tools/game_proxy/relay.py ]]; then
  scp tools/game_proxy/relay.py tools/master_proxy/sessions.py tools/game_proxy/cod2-game-relay.service \
      tools/game_proxy/requirements.txt \
      tools/nginx-cod2-web.conf \
      "$HOST:/tmp/cod2-game-relay-upload/" || true
fi

ssh "$HOST" "VER='$VER' DST='$DST' PUBLISH_REMOTE_ASSETS='${PUBLISH_REMOTE_ASSETS:-0}' bash -s" <<'EOF'
set -eu
set -o pipefail
SRC=/tmp/cod2-web-upload
mkdir -p "$SRC" "$DST" "$DST/r3"
# Live client is /play/r3/ (bookmarks + root redirect).
install -m 644 "$SRC/cod2.html"  "$DST/r3/cod2.html"
install -m 644 "$SRC/cod2.js"    "$DST/r3/cod2.js"
install -m 644 "$SRC/cod2.wasm"  "$DST/r3/cod2.wasm"
gzip -9kf "$DST/r3/cod2.wasm"
install -m 644 "$SRC/cod2_fs.js" "$DST/r3/cod2_fs.js"
install -m 644 "$SRC/cod2.js"    "$DST/cod2.js"
install -m 644 "$SRC/cod2.wasm"  "$DST/cod2.wasm"
gzip -9kf "$DST/cod2.wasm"
install -m 644 "$SRC/cod2_fs.js" "$DST/cod2_fs.js"
for f in lobby.js brand-mark.png favicon.png apple-touch-icon.png; do
  [[ -f "$SRC/$f" ]] || continue
  install -m 644 "$SRC/$f" "$DST/$f"
  install -m 644 "$SRC/$f" "$DST/r3/$f"
done
# Remote game assets are OFF by default: original game archives must not be
# served from an open endpoint. Set PUBLISH_REMOTE_ASSETS=1 only for a pack
# you have the right to distribute, or behind access control (see
# docs/STAGE2_DELIVERY.md). Players otherwise load their own files.
if [[ "${PUBLISH_REMOTE_ASSETS:-0}" == "1" ]]; then
  REMOTE_ASSETS_JS="window.COD2_REMOTE_ASSETS = { manifestUrl: '/play/assets.json', baseUrl: '/' };"
else
  REMOTE_ASSETS_JS="/* remote assets disabled (PUBLISH_REMOTE_ASSETS!=1) */"
fi
cat > "$DST/remote-config.js" <<JS
$REMOTE_ASSETS_JS
window.COD2_MASTER_URL = '/play/api/masterlist/cod2/1.3';
window.COD2_RELAY_URL = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/play/api/gamews';
JS
cp -f "$DST/remote-config.js" "$DST/r3/remote-config.js"

# Install/refresh game relay + ensure nginx has /play/api/gamews.
if [[ -f /tmp/cod2-game-relay-upload/relay.py ]]; then
  mkdir -p /opt/cod2-game-relay
  install -m 644 /tmp/cod2-game-relay-upload/relay.py /opt/cod2-game-relay/relay.py
  install -m 644 /tmp/cod2-game-relay-upload/sessions.py /opt/cod2-game-relay/sessions.py
  if [[ -f /tmp/cod2-game-relay-upload/cod2-game-relay.service ]]; then
    install -m 644 /tmp/cod2-game-relay-upload/cod2-game-relay.service /etc/systemd/system/cod2-game-relay.service
    systemctl daemon-reload || true
    systemctl enable --now cod2-game-relay || true
  fi
  if ! python3 -c 'import websockets' 2>/dev/null; then
    apt-get install -y python3-websockets >/dev/null 2>&1 || \
      python3 -m pip install --quiet websockets 2>/dev/null || true
  fi
  # Inject gamews into live vhost (sites-available/cod2 on this VPS).
  for conf in /etc/nginx/sites-available/cod2 /etc/nginx/sites-available/cod2-web \
              /etc/nginx/sites-enabled/cod2 /etc/nginx/sites-enabled/default; do
    [[ -f "$conf" ]] || continue
    if grep -q 'location /play/api/gamews' "$conf"; then
      echo "nginx: gamews already in $conf"
      break
    fi
    if grep -q 'location /play/api/' "$conf"; then
      python3 - "$conf" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
t = p.read_text()
needle = "    location /play/api/ {"
block = """    location /play/api/gamews {
        proxy_pass http://127.0.0.1:20712/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_connect_timeout 5s;
        proxy_read_timeout 300s;
        proxy_send_timeout 300s;
    }

"""
if needle in t and "location /play/api/gamews" not in t:
    p.write_text(t.replace(needle, block + needle, 1))
    print("injected gamews into", p)
else:
    print("skip inject", p)
PY
      break
    fi
  done
  if ! grep -rq 'location /play/api/gamews' /etc/nginx/sites-available /etc/nginx/sites-enabled /etc/nginx/conf.d 2>/dev/null; then
    echo "WARN: /play/api/gamews not found in nginx — add tools/game_proxy/nginx-gamews.conf.snippet"
  fi
  /usr/sbin/nginx -t && systemctl reload nginx || true
  systemctl restart cod2-game-relay || true
fi
python3 - <<PY
from pathlib import Path
import re, os
ver = os.environ["VER"]
dst = Path(os.environ["DST"])
p = dst / "r3" / "cod2.html"
h = p.read_text(encoding="utf-8", errors="replace")
h = re.sub(r"ENGINE_ASSET_VER\s*=\s*'[^']*'", f"ENGINE_ASSET_VER = '{ver}'", h)
h = re.sub(r'var ue="[^"]+"', f'var ue="{ver}"', h)
h = re.sub(r"o1-[a-z0-9-]+-20260[0-9]+", ver, h)
h = re.sub(r'src=["\']?cod2\.js(\?v=[^"\']*)?["\']?', f'src="cod2.js?v={ver}"', h)
h = re.sub(r'src=["\']?cod2_fs\.js(\?v=[^"\']*)?["\']?', f'src="cod2_fs.js?v={ver}"', h)
h = re.sub(r'src=["\']?remote-config\.js(\?v=[^"\']*)?["\']?', f'src="remote-config.js?v={ver}"', h)
if "remote-config.js" not in h:
    h = h.replace(
        '<script src="cod2_fs.js',
        '<script src="remote-config.js?v=' + ver + '"></script>\n  <script src="cod2_fs.js',
        1,
    )
p.write_text(h, encoding="utf-8")
print("patched", p, "ver", ver)
PY
cat > "$DST/cod2.html" <<HTML
<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=/play/r3/cod2.html?v=${VER}"><title>CoD2</title></head>
<body><a href="/play/r3/cod2.html?v=${VER}">Open CoD2</a></body></html>
HTML
ls -lah "$DST/r3"
md5sum "$DST/r3/cod2.wasm"
echo "OPEN https://${HOSTONLY}/play/r3/cod2.html?v=${VER}"
EOF

echo "Deployed. Open https://HOST/play/r3/cod2.html?v=${VER}"
