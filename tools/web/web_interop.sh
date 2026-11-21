#!/bin/bash
# curl against ww's HTTP server (tools/web/web_peer.ww), over HTTP and HTTPS: GET, HEAD, bodies by
# length and in chunks, 100-continue, gzip, keep-alive, HTTP/1.0, forms, multipart, cookies,
# redirects, files, limits and malformed requests. Run from the repository's root, with curl and
# OpenSSL on the PATH:
#
#   bash tools/web/web_interop.sh [ww]          (Git Bash on Windows: build/clang/ww.exe; Linux: ./ww)
set -u
ww=${1:-build/clang/ww.exe}
work=$(mktemp -d)
if command -v cygpath >/dev/null; then work=$(cygpath -m "$work"); fi
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT
export MSYS_NO_PATHCONV=1
port=${WEB_PORT:-47711}
tlsport=$((port + 1))
passed=0
failed=0
revoke=$(curl --version | grep -q Schannel && echo --ssl-no-revoke)

# A certificate for 127.0.0.1 and localhost, under a throwaway root.
cat > "$work/ext.cnf" <<'EOF'
[req]
distinguished_name = dn
[dn]
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign
[server]
basicConstraints = critical, CA:FALSE
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost, IP:127.0.0.1
EOF
q() { "$@" >/dev/null 2>&1; }
q openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/root.key"
q openssl req -new -x509 -key "$work/root.key" -subj "/CN=Web Root" -days 30 -config "$work/ext.cnf" -extensions ca -out "$work/root.pem"
q openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/server.key"
q openssl req -new -key "$work/server.key" -subj "/CN=localhost" -config "$work/ext.cnf" -out "$work/server.csr"
q openssl x509 -req -in "$work/server.csr" -CA "$work/root.pem" -CAkey "$work/root.key" -set_serial 7 -days 30 -extfile "$work/ext.cnf" -extensions server -out "$work/server.pem"
q openssl pkcs12 -export -inkey "$work/server.key" -in "$work/server.pem" -passout pass:pw -out "$work/server.p12"

"$ww" tools/web/web_peer.ww $port --seconds 300 > "$work/plain.log" 2>&1 &
"$ww" tools/web/web_peer.ww $tlsport --tls "$work/server.p12" pw --seconds 300 > "$work/tls.log" 2>&1 &
for i in $(seq 1 100); do
  grep -q listening "$work/plain.log" 2>/dev/null && grep -q listening "$work/tls.log" 2>/dev/null && break
  sleep 0.1
done

check() { # name, pattern, output
  if echo "$3" | grep -qE "$2"; then
    passed=$((passed + 1)); echo "PASS $1"
  else
    failed=$((failed + 1)); echo "FAIL $1"; echo "$3" | head -12 | sed 's/^/    /'
  fi
}
base="http://127.0.0.1:$port"
c() { curl -sS --max-time 20 "$@" 2>&1; }

for scheme in http https; do
  if [ $scheme = https ]; then b="https://localhost:$tlsport"; t="--cacert $work/root.pem $revoke"; else b=$base; t=""; fi
  check "$scheme GET" "^hello$" "$(c $t $b/hello)"
  check "$scheme JSON" '"id": "42"' "$(c $t "$b/users/42?q=wolf")"
  check "$scheme HEAD" "Content-Length: 5" "$(c $t -I $b/hello)"
  check "$scheme 404" "404" "$(c $t -o "$work/null" -w '%{http_code}' $b/nowhere)"
  check "$scheme gzip" "Content-Encoding: gzip" "$(c $t --compressed -D - -o "$work/null" $b/big)"
  check "$scheme gzip decoded" "^204800$" "$(c $t --compressed $b/big | wc -c | tr -d ' ')"
  check "$scheme keep-alive" "Re-using existing connection|Reusing existing" "$(c $t -v $b/hello $b/hello 2>&1)"
  check "$scheme redirect" "^hello$" "$(c $t -L $b/redirect)"
  check "$scheme chunked response" "line 9" "$(c $t $b/stream)"
done

head -c 3000000 /dev/urandom > "$work/data.bin"
want=$(openssl dgst -sha256 "$work/data.bin" | sed 's/.*= //')
check "POST 3 MB (100-continue)" "\"sha256\": \"$want\"" "$(c -H 'Content-Type: application/octet-stream' --data-binary @"$work/data.bin" $base/echo)"
check "POST in chunks" "\"sha256\": \"$want\"" "$(c -H 'Transfer-Encoding: chunked' -H 'Content-Type: application/octet-stream' --data-binary @"$work/data.bin" $base/echo)"
check "POST over HTTPS" "\"sha256\": \"$want\"" "$(c --cacert "$work/root.pem" $revoke -H 'Content-Type: application/octet-stream' --data-binary @"$work/data.bin" https://localhost:$tlsport/echo)"
check "a form" '"b": "x&y"' "$(c --data-urlencode 'a=1 2' --data-urlencode 'b=x&y' $base/form)"
printf 'data.txt body' > "$work/data.txt"
check "multipart with a file" '"f": "data.txt body"' "$(c -F 'n=v' -F "f=@$work/data.txt" $base/form)"
check "cookies" '"wolf": "pack"' "$(c -c "$work/jar" $base/cookie >/dev/null; c -b "$work/jar" $base/cookies)"
check "HTTP/1.0" "HTTP/1.1 200" "$(c --http1.0 -i $base/hello)"
check "a file, with Last-Modified" "Last-Modified:" "$(c -I $base/files/page.html)"
check "304 Not Modified" "^304$" "$(c -o "$work/null" -w '%{http_code}' -H 'If-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT' $base/files/page.html)"
check "a file outside refused" "^404$" "$(c -o "$work/null" -w '%{http_code}' --path-as-is $base/files/../../CMakeLists.txt)"
check "405 with Allow" "Allow: GET" "$(c -i -X DELETE $base/hello)"
check "a request line too long" "^414$" "$(c -o "$work/null" -w '%{http_code}' "$base/hello?$(head -c 9000 /dev/zero | tr '\0' a)")"
printf 'X-Big: %s\n' "$(head -c 70000 /dev/zero | tr '\0' a)" > "$work/big-header"
check "headers too large" "^431$" "$(c -o "$work/null" -w '%{http_code}' -H @"$work/big-header" $base/hello)"
check "both lengths refused (smuggling)" "400" "$(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' | openssl s_client -quiet -connect 127.0.0.1:$tlsport -servername localhost 2>/dev/null | head -1)"

echo "---- $passed passed, $failed failed ----"
[ $failed = 0 ]
