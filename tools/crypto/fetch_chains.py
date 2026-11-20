# Fetches the certificate chains of a few public HTTPS sites for tests/certificates_real.ww: the
# chain each serves (openssl s_client -showcerts), the root it ends at (found among the system's
# roots, Ubuntu's /etc/ssl/certs/ca-certificates.crt), and the time it was fetched, at which the
# test checks the chain, so that it still holds once the certificates have expired. Run in WSL:
#
#   wsl -e python3 tools/crypto/fetch_chains.py tests/data/certs/real
import datetime, json, os, re, subprocess, sys

from cryptography import x509

sites = ["www.google.com", "github.com", "www.cloudflare.com", "letsencrypt.org", "www.wikipedia.org", "www.microsoft.com", "www.amazon.com", "www.mozilla.org"]
folder = sys.argv[1]
os.makedirs(folder, exist_ok=True)

with open("/etc/ssl/certs/ca-certificates.crt", "rb") as f:
    roots = x509.load_pem_x509_certificates(f.read())

index = []
for host in sites:
    shown = subprocess.run(["openssl", "s_client", "-connect", host + ":443", "-servername", host, "-showcerts"], input=b"", capture_output=True, timeout=30).stdout.decode("latin-1")
    pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", shown, re.S)
    chain = [x509.load_pem_x509_certificate(p.encode()) for p in pems]
    # The shortest path to a root: stop at the first certificate that is a root (the same subject
    # and key) or was issued by one.
    from cryptography.hazmat.primitives import serialization

    def key_bytes(c):
        return c.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)

    root, used = None, len(chain)
    for i, cert in enumerate(chain):
        same = [r for r in roots if r.subject == cert.subject and key_bytes(r) == key_bytes(cert)]
        if same:
            root, used = same[0], i
            break
        for candidate in roots:
            if candidate.subject == cert.issuer:
                try:
                    cert.verify_directly_issued_by(candidate)
                    root, used = candidate, i + 1
                    break
                except Exception:
                    pass
        if root is not None:
            break
    if root is None:
        raise SystemExit(host + ": no root of the system's found for the chain")
    pems = pems[:used]
    chain = chain[:used]

    name = host.replace("www.", "").split(".")[0]
    with open(os.path.join(folder, name + ".pem"), "w", newline="\n") as f:
        for p in pems:
            f.write(p.strip() + "\n")
    with open(os.path.join(folder, name + "-root.pem"), "w", newline="\n") as f:
        from cryptography.hazmat.primitives import serialization
        f.write(root.public_bytes(serialization.Encoding.PEM).decode())
    when = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    index.append({"host": host, "chain": name + ".pem", "root": name + "-root.pem", "fetched": when.isoformat().replace("+00:00", "Z"),
                  "leafKey": chain[0].public_key().__class__.__name__, "rootSubject": root.subject.rfc4514_string()})
    print(host, len(chain), "certificates, root", root.subject.rfc4514_string())

with open(os.path.join(folder, "index.json"), "w", newline="\n") as f:
    json.dump(index, f, indent=1)
    f.write("\n")
