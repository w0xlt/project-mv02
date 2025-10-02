#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import urllib.request
import urllib.error

def run_cli(cli_path, *args, timeout=120):
    """Run bitcoin-cli and return parsed JSON (if possible) or raw text."""
    cmd = [cli_path, *args]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Command failed: {' '.join(cmd)}\nstdout+stderr:\n{e.output.decode(errors='ignore')}", file=sys.stderr)
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print(f"[ERROR] Command timed out: {' '.join(cmd)}", file=sys.stderr)
        sys.exit(1)

    txt = out.decode().strip()
    # Most bitcoin-cli outputs here are JSON; try to parse, else return text.
    try:
        return json.loads(txt)
    except json.JSONDecodeError:
        return txt

def http_post_json(url, payload: dict, timeout=30):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        url=url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode()
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return body  # non-JSON response
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="ignore")
        raise RuntimeError(f"HTTP {e.code}: {body}") from e
    except urllib.error.URLError as e:
        raise RuntimeError(f"HTTP request failed: {e}") from e

def main():
    ap = argparse.ArgumentParser(description="Verify all mempool transactions with external /verify endpoint.")
    ap.add_argument("--cli", default="./build/bin/bitcoin-cli", help="Path to bitcoin-cli (default: ./build/bin/bitcoin-cli)")
    ap.add_argument("--verify-url", default="http://127.0.0.1:8080/verify", help="Verification endpoint URL (default: http://127.0.0.1:8080/verify)")
    ap.add_argument("--timeout", type=int, default=30, help="HTTP timeout seconds per verify call (default: 30)")
    ap.add_argument("--limit", type=int, default=0, help="If >0, only process this many txids (useful for quick tests).")
    ap.add_argument("--verbose", action="store_true", help="Print progress.")
    args = ap.parse_args()

    # 1) Get mempool entries with details
    if args.verbose:
        print("[*] Fetching mempool with details ...")
    mempool = run_cli(args.cli, "getrawmempool", "true")
    if not isinstance(mempool, dict):
        print("[ERROR] Unexpected getrawmempool output (expected JSON object).", file=sys.stderr)
        sys.exit(1)

    # Equivalent filter to: jq -r 'to_entries[] | select((.value.depends|length)==0) | .key'
    txids = [txid for txid, meta in mempool.items() if isinstance(meta, dict) and len(meta.get("depends", [])) == 0]

    if args.limit and args.limit > 0:
        txids = txids[:args.limit]

    if args.verbose:
        print(f"[*] Found {len(txids)} top-level (no-depends) transactions to verify.")

    # 2) For each txid: getrawtransaction, POST to /verify with {"tx_hex": "..."}
    processed = 0
    for txid in txids:
        if args.verbose and processed % 100 == 0:
            print(f"    ... at {processed}/{len(txids)}")

        raw = run_cli(args.cli, "getrawtransaction", txid)
        if not isinstance(raw, str) or not raw:
            print(f"[FAIL] getrawtransaction returned empty/non-text for {txid}", file=sys.stderr)
            print(f"[RESULT] First failing TXID: {txid}")
            sys.exit(2)

        try:
            resp = http_post_json(args.verify_url, {"tx_hex": raw}, timeout=args.timeout)
        except Exception as e:
            print(f"[FAIL] HTTP error verifying {txid}: {e}", file=sys.stderr)
            print(f"[RESULT] First failing TXID: {txid}")
            sys.exit(2)

        # 3) Validate exact match {"verification":"Test"}
        if not (isinstance(resp, dict) and resp.get("verification") == "Test"):
            print(f"[FAIL] Verification mismatch for {txid}. Got: {resp}", file=sys.stderr)
            print(f"[RESULT] First failing TXID: {txid}")
            sys.exit(3)

        processed += 1

    print(f"[OK] All {processed} transactions passed.")

if __name__ == "__main__":
    main()
