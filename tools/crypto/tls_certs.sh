#!/bin/bash
# Makes tests/data/certs/tls for tests/tls.ww: a root (P-256), and under it servers for server.test
# and 127.0.0.1 with P-256, RSA-2048 and Ed25519 keys, one that expired, and a client; each a
# PKCS#12 file with the password "pw". With OpenSSL 3.4 or later (for -not_before), in WSL or on
# Linux, from the repository's root:  bash tools/crypto/tls_certs.sh
# Every certificate is good from 2026-01-01 to 2046-01-01 but the expired one (2026 only); the
# keys are made for these tests and used for nothing else.
set -euo pipefail
out=tests/data/certs/tls
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
rm -f "$out"/*.p12 "$out"/*.pem
from=20260101000000Z
to=20460101000000Z
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
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:server.test, IP:127.0.0.1
authorityKeyIdentifier = keyid
[client]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = clientAuth
subjectAltName = DNS:client.test
authorityKeyIdentifier = keyid
EOF
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/root.key"
openssl req -new -x509 -key "$work/root.key" -subj "/CN=TLS Test Root" -not_before $from -not_after $to \
  -config "$work/ext.cnf" -extensions ca -out "$out/root.pem" 2>/dev/null
make() { # name, key options, extensions, [not after]
  openssl genpkey $2 -out "$work/$1.key" 2>/dev/null
  openssl req -new -key "$work/$1.key" -subj "/CN=$1" -config "$work/ext.cnf" -out "$work/$1.csr"
  openssl x509 -req -in "$work/$1.csr" -CA "$out/root.pem" -CAkey "$work/root.key" -set_serial "0x$(openssl rand -hex 8)" \
    -not_before $from -not_after "${4:-$to}" -extfile "$work/ext.cnf" -extensions "$3" -out "$work/$1.pem" 2>/dev/null
  openssl pkcs12 -export -inkey "$work/$1.key" -in "$work/$1.pem" -certfile "$out/root.pem" -name "$1" -passout pass:pw -out "$out/$1.p12"
}
make ec "-algorithm EC -pkeyopt ec_paramgen_curve:P-256" server
make rsa "-algorithm RSA -pkeyopt rsa_keygen_bits:2048" server
make ed25519 "-algorithm ED25519" server
make expired "-algorithm EC -pkeyopt ec_paramgen_curve:P-256" server 20261231000000Z
make client "-algorithm EC -pkeyopt ec_paramgen_curve:P-256" client
ls "$out"
