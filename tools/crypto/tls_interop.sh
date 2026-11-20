#!/bin/bash
# ww's TLS against OpenSSL's s_server and s_client (and curl where there is one): both versions,
# every suite, the groups (a HelloRetryRequest both ways), ALPN, client certificates, resumption,
# a KeyUpdate, and what must be refused. Run from the repository's root, with OpenSSL 3 on the PATH:
#
#   bash tools/crypto/tls_interop.sh [ww]          (Git Bash on Windows: build/clang/ww.exe; Linux: ./ww)
#
# A throwaway PKI is made each run: a root, an EC (P-256) and an RSA server certificate for
# server.test, a client certificate. Each case prints PASS or FAIL; the end, the counts.
set -u
ww=${1:-build/clang/ww.exe}
peer=tools/crypto/tls_peer.ww
work=$(mktemp -d)
if command -v cygpath >/dev/null; then work=$(cygpath -m "$work"); fi
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT
export MSYS_NO_PATHCONV=1
port=${TLS_PORT:-47611}
passed=0
failed=0

# ---- The PKI ----
cat > "$work/ext.cnf" <<'EOF'
[req]
distinguished_name = dn
[dn]
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign
subjectKeyIdentifier = hash
[server]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
subjectAltName = DNS:server.test, IP:127.0.0.1
[client]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = clientAuth
subjectAltName = DNS:client.test
EOF
q() { "$@" >/dev/null 2>&1; }
q openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/root.key"
q openssl req -new -x509 -key "$work/root.key" -subj "/CN=Interop Root" -days 30 -config "$work/ext.cnf" -extensions ca -out "$work/root.pem"
q openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/other.key"
q openssl req -new -x509 -key "$work/other.key" -subj "/CN=Other Root" -days 30 -config "$work/ext.cnf" -extensions ca -out "$work/other.pem"
leaf() { # name, key algorithm options, extensions
  q openssl genpkey $2 -out "$work/$1.key"
  q openssl req -new -key "$work/$1.key" -subj "/CN=$1" -config "$work/ext.cnf" -out "$work/$1.csr"
  q openssl x509 -req -in "$work/$1.csr" -CA "$work/root.pem" -CAkey "$work/root.key" -set_serial "0x$(openssl rand -hex 8)" -days 30 \
    -extfile "$work/ext.cnf" -extensions "$3" -out "$work/$1.pem"
  q openssl pkcs12 -export -inkey "$work/$1.key" -in "$work/$1.pem" -passout pass:pw -out "$work/$1.p12"
}
leaf ec "-algorithm EC -pkeyopt ec_paramgen_curve:P-256" server
leaf rsa "-algorithm RSA -pkeyopt rsa_keygen_bits:2048" server
leaf client "-algorithm EC -pkeyopt ec_paramgen_curve:P-256" client

# ---- Checking ----
check() { # name, pattern, output
  if echo "$3" | grep -qE "$2"; then
    passed=$((passed + 1)); echo "PASS $1"
  else
    failed=$((failed + 1)); echo "FAIL $1"; echo "$3" | grep -E "REFUSED|tls1[23] |error|alert" | sed 's/^/    /' | head -8
  fi
}
wait_log() { # file, the line that says the server is listening (a probe would use up its accept)
  for i in $(seq 1 100); do
    grep -q "$2" "$1" 2>/dev/null && return 0
    sleep 0.1
  done
}
# ww as the client against s_server with these options; the client's own options after "--".
client_case() { # name, pattern, s_server options..., --, client options...
  local name=$1 pattern=$2; shift 2
  local server=() client=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do server+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  client=("$@")
  port=$((port + 1))
  openssl s_server -accept $port -naccept 2 -www -cert "$work/ec.pem" -key "$work/ec.key" "${server[@]}" >"$work/server.log" 2>&1 &
  local pid=$!
  wait_log "$work/server.log" ACCEPT
  local out
  out=$("$ww" $peer client 127.0.0.1 $port server.test "$work/root.pem" --send 'GET / HTTP/1.0\r\n\r\n' "${client[@]}" 2>&1)
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  check "$name" "$pattern" "$out"
}
# s_client against ww as the server with these options.
server_case() { # name, pattern, server options..., --, s_client options...
  local name=$1 pattern=$2; shift 2
  local server=() client=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do server+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  client=("$@")
  port=$((port + 1))
  "$ww" $peer server $port "$work/ec.p12" pw "${server[@]}" >"$work/peer.log" 2>&1 &
  local pid=$!
  wait_log "$work/peer.log" "listening on"
  local out
  out=$(printf 'GET / HTTP/1.0\r\n\r\n' | openssl s_client -connect 127.0.0.1:$port -servername server.test -CAfile "$work/root.pem" -verify_return_error -ign_eof "${client[@]}" 2>&1)
  wait $pid 2>/dev/null
  check "$name" "$pattern" "$out $(cat "$work/peer.log")"
}

echo "---- ww as the client, OpenSSL $(openssl version | cut -d' ' -f2) as the server ----"
client_case "1.3 default" "tls13 TLS_AES_128_GCM_SHA256 x25519.*200 ok"
for s in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
  client_case "1.3 $s" "tls13 $s .*200 ok" -ciphersuites $s
done
client_case "1.3 HelloRetryRequest for secp256r1" "tls13 .* secp256r1 .*200 ok" -groups P-256
client_case "1.3 HelloRetryRequest for secp384r1" "tls13 .* secp384r1 .*200 ok" -groups P-384
client_case "1.3 RSA-PSS certificate" "tls13 .*200 ok" -cert "$work/rsa.pem" -key "$work/rsa.key"
client_case "1.3 ALPN" "alpn=http/1.1 .*200 ok" -alpn http/1.1 -- --alpn h2,http/1.1
client_case "1.3 client certificate" "tls13 .*200 ok" -Verify 1 -CAfile "$work/root.pem" -- --identity "$work/client.p12" pw
client_case "1.3 resumed" "resumed=true" -- --repeat 2
client_case "1.3 KeyUpdate" "tls13 .*200 ok" -- --update
for s in ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305; do
  client_case "1.2 $s" "tls12 .*200 ok" -tls1_2 -cipher $s
done
for s in ECDHE-RSA-AES128-GCM-SHA256 ECDHE-RSA-AES256-GCM-SHA384 ECDHE-RSA-CHACHA20-POLY1305; do
  client_case "1.2 $s" "tls12 .*200 ok" -tls1_2 -cipher $s -cert "$work/rsa.pem" -key "$work/rsa.key"
done
client_case "1.2 secp384r1" "tls12 .* secp384r1 .*200 ok" -tls1_2 -groups P-384
client_case "1.2 client certificate" "tls12 .*200 ok" -tls1_2 -Verify 1 -CAfile "$work/root.pem" -- --identity "$work/client.p12" pw
client_case "refused: wrong name" "REFUSED: .*not for server.example" -- --name server.example
client_case "refused: untrusted root" "REFUSED: .*not trusted.*unknown_ca" -cert "$work/rsa.pem" -key "$work/rsa.key" -- --roots "$work/other.pem"
client_case "refused: 1.2 suites only of CBC" "REFUSED: .*handshake_failure" -tls1_2 -cipher ECDHE-ECDSA-AES128-SHA256
client_case "refused: 1.3 only against 1.2 only" "REFUSED: .*protocol_version" -tls1_2 -- --tls13

echo "---- OpenSSL as the client, ww as the server ----"
server_case "1.3 default" "TLSv1.3.*TLS_AES_128_GCM_SHA256"
for s in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
  server_case "1.3 $s" "Cipher is $s" -- -ciphersuites $s
done
server_case "1.3 HelloRetryRequest (the client's share ffdhe2048)" "tls13 .* secp256r1" -- -groups ffdhe2048:P-256
server_case "1.3 ALPN" "ALPN protocol: http/1.1" --alpn http/1.1 -- -alpn h2,http/1.1
server_case "1.3 client certificate" "tls13 .*peer=[12]" --client-auth required "$work/root.pem" -- -cert "$work/client.pem" -key "$work/client.key"
server_case "1.3 client certificate missing" "REFUSED: the client sent no certificate" --client-auth required "$work/root.pem"
for s in ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305; do
  server_case "1.2 $s" "Cipher is $s" -- -tls1_2 -cipher $s
done
server_case "1.2 client certificate" "tls12 .*peer=[12]" --client-auth required "$work/root.pem" -- -tls1_2 -cert "$work/client.pem" -key "$work/client.key"
server_case "refused: 1.2 fallback (SCSV)" "inappropriate fallback|REFUSED: .*fell back" -- -tls1_2 -fallback_scsv
# Resumption: the session written by the first connection, offered by the second.
port=$((port + 1))
"$ww" $peer server $port "$work/ec.p12" pw --count 2 >"$work/peer.log" 2>&1 &
pid=$!
wait_log "$work/peer.log" "listening on"
printf 'GET / HTTP/1.0\r\n\r\n' | openssl s_client -connect 127.0.0.1:$port -servername server.test -CAfile "$work/root.pem" -ign_eof -sess_out "$work/sess" >/dev/null 2>&1
out=$(printf 'GET / HTTP/1.0\r\n\r\n' | openssl s_client -connect 127.0.0.1:$port -servername server.test -CAfile "$work/root.pem" -ign_eof -sess_in "$work/sess" 2>&1)
wait $pid 2>/dev/null
check "1.3 resumed" "Reused, TLSv1.3" "$out"

if command -v curl >/dev/null; then
  echo "---- curl $(curl --version | head -1 | cut -d' ' -f2) as the client ----"
  # Schannel (Git Bash's curl) asks for revocation, which a throwaway root has not.
  revoke=$(curl --version | grep -q Schannel && echo --ssl-no-revoke)
  for v in --tlsv1.3 "--tlsv1.2 --tls-max 1.2"; do
    port=$((port + 1))
    "$ww" $peer server $port "$work/ec.p12" pw --alpn http/1.1 >"$work/peer.log" 2>&1 &
    pid=$!
    wait_log "$work/peer.log" "listening on"
    out=$(curl -sS $v $revoke --cacert "$work/root.pem" --resolve server.test:$port:127.0.0.1 "https://server.test:$port/hello" 2>&1)
    wait $pid 2>/dev/null
    check "curl $v" "request=GET /hello" "$out"
  done
fi

echo "---- $passed passed, $failed failed ----"
[ $failed = 0 ]
