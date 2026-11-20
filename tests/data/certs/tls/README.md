# TLS test certificates

Made by `tools/crypto/tls_certs.sh` with OpenSSL 3.5 for `tests/tls.ww` and
`tools/crypto/tls_fuzz.ww`, and used for nothing else: a root (`root.pem`, P-256) and, under it, in
PKCS#12 files with the password `pw` (each with the root after its own certificate):

- `ec.p12`, `rsa.p12`, `ed25519.p12`: servers for `server.test` and `127.0.0.1`, P-256, RSA-2048
  and Ed25519 keys, good from 2026-01-01 to 2046-01-01;
- `expired.p12`: the same, P-256, expired at the end of 2026;
- `client.p12`: a client (`client.test`, extended key usage clientAuth), P-256.
