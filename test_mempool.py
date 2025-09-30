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
    try:
        return json.loads(txt)
    except json.JSONDecodeError:
        return txt

def http_post_json(url, payload: dict, timeout=30):
    """POST JSON data to URL and return parsed response."""
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
                return body
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="ignore")
        raise RuntimeError(f"HTTP {e.code}: {body}") from e
    except urllib.error.URLError as e:
        raise RuntimeError(f"HTTP request failed: {e}") from e

def http_get_json(url, timeout=30):
    """GET from URL and return parsed JSON response."""
    req = urllib.request.Request(url=url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode()
            return json.loads(body)
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="ignore")
        raise RuntimeError(f"HTTP {e.code}: {body}") from e
    except urllib.error.URLError as e:
        raise RuntimeError(f"HTTP request failed: {e}") from e

def http_delete(url, timeout=30):
    """DELETE request to URL."""
    req = urllib.request.Request(url=url, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode()
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return body
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="ignore")
        raise RuntimeError(f"HTTP {e.code}: {body}") from e
    except urllib.error.URLError as e:
        raise RuntimeError(f"HTTP request failed: {e}") from e

def main():
    ap = argparse.ArgumentParser(description="Test private mempool by adding transactions and verifying ordering.")
    ap.add_argument("--cli", default="./build/bin/bitcoin-cli", help="Path to bitcoin-cli (default: ./build/bin/bitcoin-cli)")
    ap.add_argument("--base-url", default="http://127.0.0.1:8080", help="Base URL for API (default: http://127.0.0.1:8080)")
    ap.add_argument("--timeout", type=int, default=30, help="HTTP timeout seconds per request (default: 30)")
    ap.add_argument("--limit", type=int, default=10, help="Maximum number of transactions to add (default: 10)")
    ap.add_argument("--verbose", action="store_true", help="Print progress.")
    ap.add_argument("--skip-clear", action="store_true", help="Skip clearing mempool before test.")
    args = ap.parse_args()

    mempool_add_url = f"{args.base_url}/mempool/add"
    mempool_list_url = f"{args.base_url}/mempool"
    mempool_clear_url = f"{args.base_url}/mempool/clear"

    # Step 0: Clear the private mempool before starting (unless --skip-clear)
    if not args.skip_clear:
        if args.verbose:
            print("[*] Clearing private mempool...")
        try:
            clear_resp = http_post_json(mempool_clear_url, {}, timeout=args.timeout)
            if args.verbose:
                print(f"    Mempool cleared: {clear_resp}")
        except Exception as e:
            print(f"[WARN] Could not clear mempool: {e}", file=sys.stderr)

    # Step 1: Fetch Bitcoin Core mempool transactions
    if args.verbose:
        print("[*] Fetching Bitcoin Core mempool with details...")
    mempool = run_cli(args.cli, "getrawmempool", "true")
    if not isinstance(mempool, dict):
        print("[ERROR] Unexpected getrawmempool output (expected JSON object).", file=sys.stderr)
        sys.exit(1)

    # Filter for transactions with no dependencies
    txids = [txid for txid, meta in mempool.items() if isinstance(meta, dict) and len(meta.get("depends", [])) == 0]

    if args.limit and args.limit > 0:
        txids = txids[:args.limit]

    if args.verbose:
        print(f"[*] Found {len(txids)} top-level (no-depends) transactions.")

    if len(txids) == 0:
        print("[WARN] No transactions to test. Mempool might be empty.", file=sys.stderr)
        sys.exit(0)

    # Step 2: Add each transaction to the private mempool
    if args.verbose:
        print(f"[*] Adding {len(txids)} transactions to private mempool...")

    added_txs = []
    for idx, txid in enumerate(txids):
        if args.verbose and idx % 10 == 0:
            print(f"    ... at {idx}/{len(txids)}")

        # Get raw transaction hex
        raw = run_cli(args.cli, "getrawtransaction", txid)
        if not isinstance(raw, str) or not raw:
            print(f"[FAIL] getrawtransaction returned empty/non-text for {txid}", file=sys.stderr)
            sys.exit(2)

        # Add to private mempool
        try:
            resp = http_post_json(mempool_add_url, {"tx_hex": raw}, timeout=args.timeout)
        except Exception as e:
            print(f"[FAIL] HTTP error adding {txid} to mempool: {e}", file=sys.stderr)
            print(f"[RESULT] First failing TXID: {txid}")
            sys.exit(2)

        # Validate response format
        if not isinstance(resp, dict):
            print(f"[FAIL] Response is not a JSON object for {txid}. Got: {resp}", file=sys.stderr)
            sys.exit(3)

        # Check required fields
        required_fields = ["txid", "fee_sats", "size_bytes", "fee_rate", "mempool_size"]
        for field in required_fields:
            if field not in resp:
                print(f"[FAIL] Missing field '{field}' in response for {txid}", file=sys.stderr)
                sys.exit(3)

        # Validate txid matches
        if resp["txid"] != txid:
            print(f"[FAIL] txid mismatch. Expected {txid} but got {resp['txid']}", file=sys.stderr)
            sys.exit(3)

        # Validate types
        if not isinstance(resp["fee_sats"], int):
            print(f"[FAIL] fee_sats is not an integer for {txid}. Got: {resp['fee_sats']}", file=sys.stderr)
            sys.exit(3)

        if not isinstance(resp["size_bytes"], int):
            print(f"[FAIL] size_bytes is not an integer for {txid}. Got: {resp['size_bytes']}", file=sys.stderr)
            sys.exit(3)

        if not isinstance(resp["fee_rate"], (int, float)):
            print(f"[FAIL] fee_rate is not a number for {txid}. Got: {resp['fee_rate']}", file=sys.stderr)
            sys.exit(3)

        # Validate mempool_size is increasing
        if resp["mempool_size"] != len(added_txs) + 1:
            print(f"[FAIL] mempool_size mismatch. Expected {len(added_txs) + 1} but got {resp['mempool_size']}", file=sys.stderr)
            sys.exit(3)

        added_txs.append({
            "txid": resp["txid"],
            "fee_sats": resp["fee_sats"],
            "size_bytes": resp["size_bytes"],
            "fee_rate": resp["fee_rate"]
        })

        if args.verbose:
            print(f"    Added: {txid[:16]}... (fee_rate: {resp['fee_rate']:.2f} sat/byte)")

    print(f"[OK] Successfully added {len(added_txs)} transactions to private mempool.")

    # Step 3: Retrieve mempool and verify ordering
    if args.verbose:
        print("[*] Retrieving private mempool to verify ordering...")

    try:
        mempool_resp = http_get_json(mempool_list_url, timeout=args.timeout)
    except Exception as e:
        print(f"[FAIL] Failed to retrieve mempool: {e}", file=sys.stderr)
        sys.exit(4)

    if not isinstance(mempool_resp, dict) or "transactions" not in mempool_resp:
        print(f"[FAIL] Invalid mempool response format. Got: {mempool_resp}", file=sys.stderr)
        sys.exit(4)

    transactions = mempool_resp["transactions"]
    count = mempool_resp.get("count", 0)

    if count != len(added_txs):
        print(f"[FAIL] Mempool count mismatch. Expected {len(added_txs)} but got {count}", file=sys.stderr)
        sys.exit(4)

    if len(transactions) != len(added_txs):
        print(f"[FAIL] Transaction list length mismatch. Expected {len(added_txs)} but got {len(transactions)}", file=sys.stderr)
        sys.exit(4)

    # Step 4: Verify transactions are ordered by fee rate (descending)
    if args.verbose:
        print("[*] Verifying fee rate ordering (highest first)...")

    for i in range(len(transactions) - 1):
        curr_fee_rate = transactions[i].get("fee_rate", 0)
        next_fee_rate = transactions[i + 1].get("fee_rate", 0)

        if curr_fee_rate < next_fee_rate:
            print(f"[FAIL] Transactions not ordered by fee rate!", file=sys.stderr)
            print(f"    Transaction at index {i} has fee_rate {curr_fee_rate}", file=sys.stderr)
            print(f"    Transaction at index {i+1} has fee_rate {next_fee_rate}", file=sys.stderr)
            print(f"    Expected descending order (highest fee rate first)", file=sys.stderr)
            sys.exit(5)

    print(f"[OK] All {len(transactions)} transactions correctly ordered by fee rate (descending).")

    # Step 5: Test removing a transaction
    if len(transactions) > 0 and args.verbose:
        test_txid = transactions[0]["txid"]
        print(f"[*] Testing removal of transaction: {test_txid[:16]}...")

        delete_url = f"{args.base_url}/mempool/{test_txid}"
        try:
            delete_resp = http_delete(delete_url, timeout=args.timeout)
            if args.verbose:
                print(f"    Removed: {delete_resp}")

            # Verify mempool size decreased
            mempool_resp = http_get_json(mempool_list_url, timeout=args.timeout)
            new_count = mempool_resp.get("count", 0)
            if new_count != count - 1:
                print(f"[FAIL] Mempool size after deletion. Expected {count - 1} but got {new_count}", file=sys.stderr)
                sys.exit(6)

            print(f"[OK] Successfully removed transaction. New mempool size: {new_count}")

        except Exception as e:
            print(f"[WARN] Could not test deletion: {e}", file=sys.stderr)

    # Step 6: Display fee rate statistics
    if args.verbose and len(transactions) > 0:
        fee_rates = [tx["fee_rate"] for tx in transactions]
        print(f"\n[*] Fee rate statistics:")
        print(f"    Highest: {max(fee_rates):.2f} sat/byte")
        print(f"    Lowest: {min(fee_rates):.2f} sat/byte")
        print(f"    Average: {sum(fee_rates) / len(fee_rates):.2f} sat/byte")

    print("\n[SUCCESS] All private mempool tests passed!")

if __name__ == "__main__":
    main()