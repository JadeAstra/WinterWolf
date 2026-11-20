#!/bin/bash
# PKCS#12 files and import Crypto, both ways. Run from the repository's root where both OpenSSL 3
# and Java's keytool are on the PATH (Git Bash on Windows with a JDK has both):
#
#   bash tools/crypto/pkcs12.sh make            # tests/data/certs/pkcs12: files OpenSSL and keytool wrote
#   bash tools/crypto/pkcs12.sh check [ww.exe]  # ww writes files; OpenSSL and keytool must read them
#
# `make` writes, with the password "sesame":
#   openssl.p12         a P-384 key and its chain of three (leaf, intermediate, root), OpenSSL's defaults
#   openssl-key.pem     that key, unencrypted, to compare with
#   openssl-legacy.p12  the same in the older form (SHA-1 and triple DES), which ww refuses
#   keytool.p12         keytool's: an EC key ("ec"), an RSA key ("rsa"), a trusted certificate ("root")
#   keytool-keys.pem    its two keys, unencrypted (taken out with OpenSSL), to compare with
#   root.pem            the root of openssl.p12's chain
set -euo pipefail
mode=${1:-}
out=tests/data/certs/pkcs12
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# On Windows, a path Windows' programs take as it is (the subjects below are passed with no path
# conversion, so that "/CN=..." stays as written).
if command -v cygpath >/dev/null; then work=$(cygpath -m "$work"); fi

if [ "$mode" = make ]; then
  mkdir -p "$out"
  rm -f "$out"/*.p12 "$out"/*.pem
  cat > "$work/ext.cnf" <<'EOF'
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:server.example.test
authorityKeyIdentifier = keyid
EOF
  { printf '[req]\ndistinguished_name=dn\n[dn]\n'; cat "$work/ext.cnf"; } > "$work/req.cnf"
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/root.key"
  MSYS_NO_PATHCONV=1 openssl req -new -x509 -key "$work/root.key" -subj "/CN=PKCS12 Test Root" -days 7300 \
    -extensions ca -config "$work/req.cnf" -out "$out/root.pem"
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/inter.key"
  MSYS_NO_PATHCONV=1 openssl req -new -key "$work/inter.key" -subj "/CN=PKCS12 Test Intermediate" -out "$work/inter.csr"
  openssl x509 -req -in "$work/inter.csr" -CA "$out/root.pem" -CAkey "$work/root.key" -set_serial 2 -days 7300 \
    -extfile "$work/ext.cnf" -extensions ca -out "$work/inter.pem" 2>/dev/null
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$out/openssl-key.pem"
  MSYS_NO_PATHCONV=1 openssl req -new -key "$out/openssl-key.pem" -subj "/CN=server.example.test" -out "$work/leaf.csr"
  openssl x509 -req -in "$work/leaf.csr" -CA "$work/inter.pem" -CAkey "$work/inter.key" -set_serial 3 -days 7300 \
    -extfile "$work/ext.cnf" -extensions leaf -out "$work/leaf.pem" 2>/dev/null
  cat "$work/inter.pem" "$out/root.pem" > "$work/cas.pem"
  openssl pkcs12 -export -inkey "$out/openssl-key.pem" -in "$work/leaf.pem" -certfile "$work/cas.pem" -name server \
    -passout pass:sesame -out "$out/openssl.p12"
  openssl pkcs12 -export -inkey "$out/openssl-key.pem" -in "$work/leaf.pem" -certfile "$work/cas.pem" -name legacy \
    -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 -passout pass:sesame -out "$out/openssl-legacy.p12"
  keytool -genkeypair -keystore "$out/keytool.p12" -storetype PKCS12 -storepass sesame -alias ec -keyalg EC \
    -groupname secp256r1 -dname "CN=keytool EC" -validity 7300 >/dev/null 2>&1
  keytool -genkeypair -keystore "$out/keytool.p12" -storetype PKCS12 -storepass sesame -alias rsa -keyalg RSA \
    -keysize 2048 -dname "CN=keytool RSA" -validity 7300 >/dev/null 2>&1
  keytool -importcert -noprompt -keystore "$out/keytool.p12" -storetype PKCS12 -storepass sesame -alias root \
    -file "$out/root.pem" >/dev/null 2>&1
  openssl pkcs12 -in "$out/keytool.p12" -nocerts -nodes -passin pass:sesame -out "$out/keytool-keys.pem"
  ls "$out"
  exit 0
fi

if [ "$mode" = check ]; then
  ww=${2:-build/clang/ww.exe}
  cat > "$work/write.ww" <<'EOF'
import Crypto.*
val dir = Program.arguments[0]
val work = Program.arguments[1]
val entries = Pkcs12.read(FilePath("$dir/openssl.p12").readBytes(), "sesame") + Pkcs12.read(FilePath("$dir/keytool.p12").readBytes(), "sesame")
FilePath("$work/ours.p12").writeBytes(Pkcs12.write(entries, "sesame"))
FilePath("$work/ours-utf8.p12").writeBytes(Pkcs12.write(entries, "pass wörd ü", 1000))
print("written: ${entries.map { it.name }}")
EOF
  "$ww" "$work/write.ww" "$out" "$work"
  fail=0
  keys=$(openssl pkcs12 -in "$work/ours.p12" -nodes -passin pass:sesame 2>/dev/null | grep -c 'BEGIN PRIVATE KEY' || true)
  certs=$(openssl pkcs12 -in "$work/ours.p12" -nodes -passin pass:sesame 2>/dev/null | grep -c 'BEGIN CERTIFICATE' || true)
  echo "OpenSSL: $keys keys, $certs certificates (3 and 6 belong)"
  [ "$keys" = 3 ] && [ "$certs" = 6 ] || fail=1
  printf 'pass wörd ü' > "$work/pass.txt"
  utf8=$(openssl pkcs12 -in "$work/ours-utf8.p12" -nodes -passin "file:$work/pass.txt" 2>/dev/null | grep -c 'BEGIN PRIVATE KEY' || true)
  echo "OpenSSL, a password not in ASCII: $utf8 keys (3 belong)"
  [ "$utf8" = 3 ] || fail=1
  # keytool takes passwords in ASCII alone for PBES2 (the JDK's own limit).
  listing=$(keytool -list -keystore "$work/ours.p12" -storetype PKCS12 -storepass sesame 2>&1)
  pk=$(echo "$listing" | grep -c 'PrivateKeyEntry' || true)
  tc=$(echo "$listing" | grep -c 'trustedCertEntry' || true)
  echo "keytool: $pk key entries, $tc trusted certificates (3 and 1 belong)"
  [ "$pk" = 3 ] && [ "$tc" = 1 ] || fail=1
  chain=$(keytool -list -v -alias server -keystore "$work/ours.p12" -storetype PKCS12 -storepass sesame 2>&1 | grep 'Certificate chain length' || true)
  echo "keytool, server: $chain (3 belongs)"
  echo "$chain" | grep -q 'length: 3' || fail=1
  [ $fail = 0 ] && echo "all read" || { echo "NOT ALL READ"; exit 1; }
  exit 0
fi

echo "bash tools/crypto/pkcs12.sh make|check [ww.exe]"
exit 2
