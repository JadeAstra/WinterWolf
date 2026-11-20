# Keeps the part of x509-limbo (github.com/C2SP/x509-limbo, Apache 2.0) that tests/certificates_limbo.ww
# runs: every case but bettertls's, those that need revocation lists (CRLs, which import Crypto
# does not check) and those with ML-DSA keys; and every 20th of bettertls's thousands of name
# constraint cases. Run in WSL with limbo.json downloaded beside it:
#
#   wsl -e python3 tools/crypto/trim_limbo.py /tmp/limbo/limbo.json tests/data/limbo/limbo_subset.json
import json, sys

data = json.load(open(sys.argv[1]))
kept = []
bettertls = 0
for case in data["testcases"]:
    family = case["id"].split("::")[0]
    features = case.get("features") or []
    if "has-crl" in features or "has-mldsa" in features or case.get("crls"):
        continue
    if family == "bettertls":
        bettertls += 1
        if bettertls % 20 != 0:
            continue
    keep = {k: case[k] for k in ["id", "description", "conflicts_with", "features", "validation_kind", "trusted_certs", "untrusted_intermediates",
                                 "peer_certificate", "validation_time", "signature_algorithms", "key_usage", "extended_key_usage", "expected_result",
                                 "expected_peer_name", "expected_peer_names", "max_chain_depth"]}
    kept.append(keep)
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
    json.dump({"version": data["version"], "testcases": kept}, f, separators=(",", ":"))
    f.write("\n")
print(len(kept), "cases kept of", len(data["testcases"]))
