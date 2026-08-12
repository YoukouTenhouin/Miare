#!/usr/bin/env python3

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate.py <benchmark.json>", file=sys.stderr)
        return 2
    report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    maximums = {
        "cache_hit_lookup_p99_us": 200.0,
        "transaction_100_p99_ms": 50.0,
        "file_amplification": 1.25,
        "no_op_checkpoint_p99_ms": 1.0,
        "peak_database_owned_bytes": 64 * 1024 * 1024 + 128 * 1024 * 1024,
    }
    minimums = {
        "ordered_scan_mib_s": 100.0,
        "blob_write_mib_s": 200.0,
        "blob_read_mib_s": 200.0,
        "verification_mib_s": 150.0,
        "backup_mib_s": 150.0,
    }
    failures = []
    for metric, limit in maximums.items():
        if metric not in report:
            failures.append(f"missing metric: {metric}")
        elif report[metric] > limit:
            failures.append(f"{metric}={report[metric]} exceeds {limit}")
    for metric, limit in minimums.items():
        if metric not in report:
            failures.append(f"missing metric: {metric}")
        elif report[metric] < limit:
            failures.append(f"{metric}={report[metric]} is below {limit}")
    if failures:
        print("benchmark qualification failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("benchmark qualification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
