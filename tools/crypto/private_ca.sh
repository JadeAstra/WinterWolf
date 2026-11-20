#!/bin/bash
# Makes tests/data/certs/private: a private CA's chains, good and bad, with OpenSSL (3.4 or later,
# for -not_before), for tests/certificates_private.ww. Run from the repository's root, in WSL or
# on Linux:  bash tools/crypto/private_ca.sh
# Every certificate is good from 2026-01-01 to 2046-01-01 unless a case says otherwise; the test
# checks them at 2027-01-01. The private keys are thrown away.
set -euo pipefail
out=tests/data/certs/private
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
rm -f "$out"/*.pem
cd "$work"

from=20260101000000Z
to=20460101000000Z

cat > ext.cnf <<'EOF'
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[ca0]
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[nosign]
basicConstraints = critical, CA:TRUE
keyUsage = critical, digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[nc]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
nameConstraints = critical, permitted;DNS:example.test, excluded;DNS:secret.example.test
[leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = DNS:www.example.test, DNS:example.test, IP:192.0.2.7, IP:2001:db8::7
[wild]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:*.apps.example.test
[client]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = clientAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:client.example.test
[plain]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:www.example.test
[ok]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:a.example.test
[evil]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:evil.test
[secret]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:x.secret.example.test
[unknown]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
subjectAltName = DNS:www.example.test
1.2.3.4 = critical, ASN1:NULL
[nosan]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
authorityKeyIdentifier = keyid
EOF

key() { # name, kind
  case $2 in
    p256) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1.key" ;;
    p384) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$1.key" ;;
    rsa) openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$1.key" 2>/dev/null ;;
    rsa1024) openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out "$1.key" 2>/dev/null ;;
    ed25519) openssl genpkey -algorithm ED25519 -out "$1.key" ;;
  esac
}
root() { # name, kind, CN
  key "$1" "$2"
  openssl req -new -x509 -key "$1.key" -subj "/O=WinterWolf Test/CN=$3" -not_before $from -not_after $to \
    -extensions ca -config <(printf '[req]\ndistinguished_name=dn\n[dn]\n'; cat ext.cnf) -out "$1.pem" 2>/dev/null
}
# name, kind, CN, issuer, section, [extra options...]
cert() {
  local name=$1 kind=$2 cn=$3 issuer=$4 section=$5
  shift 5
  key "$name" "$kind"
  openssl req -new -key "$name.key" -subj "/O=WinterWolf Test/CN=$cn" -out "$name.csr" 2>/dev/null
  openssl x509 -req -in "$name.csr" -CA "$issuer.pem" -CAkey "$issuer.key" -set_serial "0x$(openssl rand -hex 12)" \
    -not_before $from -not_after $to -extfile ext.cnf -extensions "$section" "$@" -out "$name.pem" 2>/dev/null
}
chain() { cat "$@"; }

root root p256 "Test Root P-256"
root root384 p384 "Test Root P-384"
cert inter rsa "Test Intermediate RSA" root ca0
cert leaf p256 "www.example.test" inter leaf
cert wild p256 "apps" inter wild
cert edleaf ed25519 "www.example.test" root384 plain
cert pss p256 "www.example.test" inter plain -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:digest
cert sha1 p256 "www.example.test" inter plain -sha1
cert expired p256 "www.example.test" inter plain -not_after 20261201000000Z
cert future p256 "www.example.test" inter plain -not_before 20280101000000Z
cert client p256 "client.example.test" inter client
cert unknown p256 "www.example.test" inter unknown
cert nosan p256 "www.example.test" inter nosan
cert weak rsa1024 "www.example.test" inter plain
# A leaf that signs another: it is not a CA.
cert byleaf p256 "www.example.test" leaf plain
# Below the intermediate's path length of 0, another CA.
cert inter2 p256 "Test Intermediate 2" inter ca
cert deep p256 "www.example.test" inter2 plain
# A CA whose key usage has no keyCertSign.
cert nosign p256 "Test No keyCertSign" root nosign
cert unsigned p256 "www.example.test" nosign plain
# Name constraints: example.test permitted, secret.example.test excluded.
cert nc p256 "Test Constrained" root nc
cert ncok p256 "a.example.test" nc ok
cert ncevil p256 "evil.test" nc evil
cert ncsecret p256 "x.secret.example.test" nc secret

cp root.pem root384.pem "$OLDPWD/$out/"
chain leaf.pem inter.pem > "$OLDPWD/$out/good.pem"
chain wild.pem inter.pem > "$OLDPWD/$out/wildcard.pem"
chain edleaf.pem > "$OLDPWD/$out/ed25519.pem"
for c in pss sha1 expired future client unknown nosan weak; do chain $c.pem inter.pem > "$OLDPWD/$out/$c.pem"; done
chain byleaf.pem leaf.pem inter.pem > "$OLDPWD/$out/byleaf.pem"
chain deep.pem inter2.pem inter.pem > "$OLDPWD/$out/pathlen.pem"
chain unsigned.pem nosign.pem > "$OLDPWD/$out/nokeycertsign.pem"
chain inter.pem > "$OLDPWD/$out/ca-as-leaf.pem"
for c in ncok ncevil ncsecret; do chain $c.pem nc.pem > "$OLDPWD/$out/$c.pem"; done
ls "$OLDPWD/$out"
