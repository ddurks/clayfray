#!/bin/sh
# Serve build-web/ over a SECURE ORIGIN, which WebGPU requires.
#
# This exists because the obvious thing does not work: `python3 -m http.server`
# on the LAN gives you http://192.168.x.x, and a browser hides navigator.gpu
# entirely on a non-secure origin. It does not warn — WebGPU simply is not
# there, which reads exactly like "this browser has no WebGPU". Only
# http://localhost (and ::1 / 127.0.0.1) and https:// count as secure.
#
#   tools/serve-web.sh            # localhost only, for this machine
#   tools/serve-web.sh tunnel     # public https URL, for a phone (needs cloudflared)
#   tools/serve-web.sh tls        # https on the LAN with a self-signed cert
#
# For a PHONE, prefer `tunnel`: cloudflared serves a real certificate, so iOS
# Safari accepts it with no warning and no profile install. `tls` works too but
# every device has to click through a certificate warning first.
set -e
cd "$(dirname "$0")/.."
DIR=build-web
PORT=${PORT:-8000}

if [ ! -f "$DIR/clayfray.html" ]; then
  echo "no $DIR/clayfray.html — build first:" >&2
  echo "  emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web" >&2
  exit 1
fi

case "${1:-local}" in
local)
  echo "http://localhost:$PORT/clayfray.html"
  echo "(localhost is a secure origin; the LAN IP is NOT — use 'tunnel' for a phone)"
  exec python3 -m http.server -d "$DIR" "$PORT"
  ;;

tunnel)
  command -v cloudflared >/dev/null || { echo "cloudflared not installed: brew install cloudflared" >&2; exit 1; }
  python3 -m http.server -d "$DIR" "$PORT" >/dev/null 2>&1 &
  HTTP_PID=$!
  trap 'kill $HTTP_PID 2>/dev/null' EXIT INT TERM
  sleep 1
  echo "starting tunnel — open the https://….trycloudflare.com URL below, and add /clayfray.html"
  exec cloudflared tunnel --url "http://localhost:$PORT"
  ;;

tls)
  CERT=.cache/webcert
  mkdir -p "$CERT"
  IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo 127.0.0.1)
  if [ ! -f "$CERT/cert.pem" ] || ! grep -q "$IP" "$CERT/ip" 2>/dev/null; then
    # SAN must carry the IP: browsers ignore CN, so a cert without the IP in
    # subjectAltName is rejected outright rather than merely warned about.
    # APPLE'S RULES, all of which iOS enforces by REJECTING the handshake
    # rather than by offering the accept dialog — a cert that misses any of
    # them reads as "connection closed unexpectedly", not as a cert warning:
    #   - ExtendedKeyUsage must contain id-kp-serverAuth   <- the one that bit us
    #   - validity <= 398 days for certs issued after 2020-09-01
    #   - SHA-256 or better, RSA >= 2048
    #   - subjectAltName must carry the IP (browsers ignore CN entirely)
    openssl req -x509 -newkey rsa:2048 -nodes -days 397 -sha256 \
      -keyout "$CERT/key.pem" -out "$CERT/cert.pem" \
      -subj "/CN=$IP" \
      -addext "subjectAltName=IP:$IP,IP:127.0.0.1,DNS:localhost" \
      -addext "extendedKeyUsage=serverAuth" \
      -addext "keyUsage=digitalSignature,keyEncipherment" \
      -addext "basicConstraints=CA:FALSE" >/dev/null 2>&1
    echo "$IP" > "$CERT/ip"
    echo "generated a self-signed cert for $IP"
  fi
  echo "https://$IP:$PORT/clayfray.html"
  echo "Every device must accept the warning once (Safari: Show Details -> visit this website)."
  exec python3 - "$DIR" "$PORT" "$CERT" <<'PY'
import http.server, functools, ssl, sys
d, port, cert = sys.argv[1], int(sys.argv[2]), sys.argv[3]
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=d)
class S(http.server.ThreadingHTTPServer):
    # A browser opens speculative connections and drops them (favicon probes,
    # TLS preflights). The default server lets that exception escape and the
    # log fills with tracebacks, which looks like the SERVER failing when it is
    # routine client behaviour. Swallow just the handshake errors.
    def handle_error(self, request, addr):
        import sys, ssl as _s
        if not isinstance(sys.exc_info()[1], (_s.SSLError, ConnectionResetError,
                                              BrokenPipeError)):
            super().handle_error(request, addr)
s = S(("0.0.0.0", port), h)
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
c.minimum_version = ssl.TLSVersion.TLSv1_2  # iOS refuses anything older
c.load_cert_chain(cert + "/cert.pem", cert + "/key.pem")
s.socket = c.wrap_socket(s.socket, server_side=True)
s.serve_forever()
PY
  ;;

*)
  echo "usage: tools/serve-web.sh [local|tunnel|tls]" >&2
  exit 2
  ;;
esac
