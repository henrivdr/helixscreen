#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture real Snapmaker U1 pause signals for issue #991.

Polls Moonraker's print_stats + virtual_sdcard and, the instant the print
state flips to "paused", records a short burst of snapshots so the firmware
reason text / exception object has time to settle. Each capture is appended
to a JSONL file and a human-readable summary is printed to the terminal.

The three signals #991 needs, per pause cause (dirty-bed AI / clog-tangle /
filament runout):
  - print_stats.message     (firmware reason text)
  - print_stats.exception   (full object -- shape is unknown until a real fault)
  - virtual_sdcard.is_active (the make-or-break gate: does a clog drop it?)

Usage:
  scripts/snapmaker-capture-pause.py                 # defaults to U1 @ 192.168.30.103
  scripts/snapmaker-capture-pause.py --host 192.168.1.50 --label clog
  scripts/snapmaker-capture-pause.py --out /tmp/u1-pauses.jsonl

Leave it running, trigger each fault scenario in turn, and read the summary
it prints. Stop with Ctrl-C. Pure stdlib -- no venv, no pip.
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from datetime import datetime

POLL_OBJECTS = "print_stats&virtual_sdcard"


def now_iso():
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def query(host, port, timeout):
    url = f"http://{host}:{port}/printer/objects/query?{POLL_OBJECTS}"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))["result"]["status"]


def summarize(status):
    ps = status.get("print_stats", {})
    vs = status.get("virtual_sdcard", {})
    return {
        "state": ps.get("state"),
        "message": ps.get("message", ""),
        "exception": ps.get("exception", {}),
        "is_active": vs.get("is_active"),
        "filename": ps.get("filename", ""),
    }


def print_capture(label, idx, snapshots):
    """Pretty-print the settled values from a pause-capture burst."""
    last = snapshots[-1]["status"]
    s = summarize(last)
    bar = "=" * 72
    print(f"\n{bar}", file=sys.stderr)
    print(f"  PAUSE CAPTURE #{idx}"
          + (f"  [label: {label}]" if label else "")
          + f"   {now_iso()}", file=sys.stderr)
    print(bar, file=sys.stderr)
    print(f"  virtual_sdcard.is_active : {s['is_active']}"
          "   <-- KEY: does a clog drop this to false?", file=sys.stderr)
    print(f"  print_stats.message      : {s['message']!r}", file=sys.stderr)
    print(f"  print_stats.exception    : {json.dumps(s['exception'])}", file=sys.stderr)
    print(f"  print_stats.filename     : {s['filename']!r}", file=sys.stderr)
    # Show whether the fields shifted across the settle window.
    msgs = {json.dumps(summarize(sn["status"])["exception"]) for sn in snapshots}
    txts = {summarize(sn["status"])["message"] for sn in snapshots}
    if len(msgs) > 1 or len(txts) > 1:
        print("  (note: message/exception changed during settle window -- "
              "see JSONL for the full burst)", file=sys.stderr)
    print(bar + "\n", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description="Capture Snapmaker U1 pause signals (#991)")
    ap.add_argument("--host", default="192.168.30.103",
                    help="Moonraker host (default: U1 @ 192.168.30.103)")
    ap.add_argument("--port", type=int, default=7125)
    ap.add_argument("--out", default="/tmp/snapmaker-pause-captures.jsonl",
                    help="JSONL output file (appended)")
    ap.add_argument("--label", default="",
                    help="Optional scenario tag stamped on captures "
                         "(e.g. dirty-bed, clog, runout)")
    ap.add_argument("--interval", type=float, default=0.4,
                    help="Poll interval seconds (default 0.4)")
    ap.add_argument("--settle", type=float, default=5.0,
                    help="Seconds to keep snapshotting after a pause (default 5)")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="Per-request HTTP timeout seconds")
    args = ap.parse_args()

    print(f"[{now_iso()}] watching {args.host}:{args.port} -> {args.out}",
          file=sys.stderr)
    print(f"[{now_iso()}] trigger each pause scenario; Ctrl-C to stop.",
          file=sys.stderr)
    if args.label:
        print(f"[{now_iso()}] captures tagged: {args.label!r}", file=sys.stderr)

    last_state = None
    capture_idx = 0
    out = open(args.out, "a", buffering=1)

    while True:
        try:
            status = query(args.host, args.port, args.timeout)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as ex:
            print(f"[{now_iso()}] query failed: {ex}", file=sys.stderr)
            time.sleep(max(args.interval, 1.0))
            continue

        state = status.get("print_stats", {}).get("state")
        if state != last_state:
            print(f"[{now_iso()}] state: {last_state} -> {state}", file=sys.stderr)

        # Edge into paused: capture a settle burst.
        if state == "paused" and last_state != "paused":
            capture_idx += 1
            snapshots = []
            t_end = time.monotonic() + args.settle
            while True:
                snapshots.append({"ts": now_iso(), "status": status})
                if time.monotonic() >= t_end:
                    break
                time.sleep(args.interval)
                try:
                    status = query(args.host, args.port, args.timeout)
                except (urllib.error.URLError, OSError, KeyError, ValueError):
                    break
            record = {
                "capture": capture_idx,
                "label": args.label,
                "captured_at": now_iso(),
                "host": args.host,
                "settled": summarize(snapshots[-1]["status"]),
                "burst": snapshots,
            }
            out.write(json.dumps(record) + "\n")
            print_capture(args.label, capture_idx, snapshots)
            # Re-read so last_state reflects post-burst reality.
            state = snapshots[-1]["status"].get("print_stats", {}).get("state")

        last_state = state
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped.", file=sys.stderr)
