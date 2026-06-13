#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture Snapmaker U1 per-tool filament signals across a load/unload cycle.

Answers the open question behind the AMS "Unload vs Eject" follow-up: after a
tool is unloaded, does its TOOLHEAD motion sensor (e{N}_filament) actually drop
to filament_detected:false, or does the filament stay parked in the buffer with
the sensor still reading true? The AMS context menu gates "Unload" on
SlotInfo.is_present() (buffer presence), so knowing which signals flip on an
unload tells us whether to re-gate Unload on the toolhead sensor instead.

For every poll it builds a compact per-tool view and prints a line ONLY when
something changes (plus the full raw status appended to JSONL), so you can leave
it running, load a tool, unload it, and read exactly which signals transitioned.

Per tool it tracks:
  - m (motion) : filament_motion_sensor e{N}_filament.filament_detected
                 -- the TOOLHEAD sensor. The signal Unload should arguably key on.
  - en        : that sensor's .enabled flag (recovery toggles it off/on)
  - ex (exist) : print_task_config.filament_exist[N]   -- buffer/channel presence
  - p (port)  : filament_feed {left,right}.extruder{N}.filament_detected
                 -- the BUFFER-side sensor
  - c (chan)  : filament_feed {...}.extruder{N}.channel_state  (loading/unloading/idle)
And the active tool from toolhead.extruder (marked with * in the header).

Usage:
  scripts/snapmaker-capture-filament.py                  # U1 @ 192.168.30.103
  scripts/snapmaker-capture-filament.py --host 192.168.1.50 --label unload-t2
  scripts/snapmaker-capture-filament.py --out /tmp/u1-filament.jsonl

Suggested run: start it, confirm the baseline line, then on the U1 LOAD T2,
wait, UNLOAD T2. The transition lines show whether e2_filament drops. Stop with
Ctrl-C. Pure stdlib -- no venv, no pip.
"""

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
import urllib.error
from datetime import datetime

NUM_TOOLS = 4

# Objects to subscribe to. Object names with spaces are percent-encoded per
# request; Moonraker returns whatever subset the printer actually has.
POLL_OBJECTS = (
    [f"filament_motion_sensor e{i}_filament" for i in range(NUM_TOOLS)]
    + ["print_task_config", "toolhead", "filament_feed left", "filament_feed right"]
)


def now_iso():
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def query(host, port, timeout):
    q = "&".join(urllib.parse.quote(obj, safe="") for obj in POLL_OBJECTS)
    url = f"http://{host}:{port}/printer/objects/query?{q}"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))["result"]["status"]


def active_tool(status):
    name = status.get("toolhead", {}).get("extruder")
    if name == "extruder":
        return 0
    if isinstance(name, str) and name.startswith("extruder"):
        try:
            return int(name[len("extruder"):])
        except ValueError:
            return None
    return None


def port_for_tool(status, i):
    """Return (filament_detected, channel_state) for tool i from whichever
    filament_feed side carries its extruder{i} entry."""
    for side in ("filament_feed left", "filament_feed right"):
        feed = status.get(side)
        if isinstance(feed, dict):
            ext = feed.get(f"extruder{i}")
            if isinstance(ext, dict):
                return ext.get("filament_detected"), ext.get("channel_state", "")
    return None, ""


def tool_view(status, i):
    sensor = status.get(f"filament_motion_sensor e{i}_filament", {})
    exist_arr = status.get("print_task_config", {}).get("filament_exist", [])
    exist = exist_arr[i] if i < len(exist_arr) else None
    port, chan = port_for_tool(status, i)
    return {
        "motion": sensor.get("filament_detected"),
        "enabled": sensor.get("enabled"),
        "exist": exist,
        "port": port,
        "chan": chan,
    }


def summarize(status):
    return {
        "active_tool": active_tool(status),
        "tools": [tool_view(status, i) for i in range(NUM_TOOLS)],
    }


def change_key(summary):
    """Stable string of just the signals we care about, for change detection."""
    return json.dumps(summary, sort_keys=True)


def b(v):
    return {True: "Y", False: "-", None: "?"}.get(v, str(v))


def format_line(summary):
    active = summary["active_tool"]
    parts = []
    for i, tv in enumerate(summary["tools"]):
        star = "*" if active == i else " "
        chan = tv["chan"] or ""
        parts.append(
            f"T{i}{star} m={b(tv['motion'])} en={b(tv['enabled'])} "
            f"ex={b(tv['exist'])} p={b(tv['port'])} c={chan or '-':<9}"
        )
    act = f"T{active}" if active is not None else "?"
    return f"active={act} | " + " | ".join(parts)


def main():
    ap = argparse.ArgumentParser(
        description="Capture Snapmaker U1 per-tool filament signals across load/unload")
    ap.add_argument("--host", default="192.168.30.103",
                    help="Moonraker host (default: U1 @ 192.168.30.103)")
    ap.add_argument("--port", type=int, default=7125)
    ap.add_argument("--out", default="/tmp/snapmaker-filament-captures.jsonl",
                    help="JSONL output file (appended)")
    ap.add_argument("--label", default="",
                    help="Optional scenario tag stamped on captures (e.g. unload-t2)")
    ap.add_argument("--interval", type=float, default=0.3,
                    help="Poll interval seconds (default 0.3)")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="Per-request HTTP timeout seconds")
    args = ap.parse_args()

    print(f"[{now_iso()}] watching {args.host}:{args.port} -> {args.out}", file=sys.stderr)
    print(f"[{now_iso()}] legend: m=toolhead motion sensor  en=sensor enabled  "
          "ex=filament_exist (buffer)  p=port/buffer sensor  c=channel_state  *=active",
          file=sys.stderr)
    print(f"[{now_iso()}] suggested: LOAD a tool, wait, then UNLOAD it; "
          "watch whether m drops. Ctrl-C to stop.", file=sys.stderr)
    if args.label:
        print(f"[{now_iso()}] captures tagged: {args.label!r}", file=sys.stderr)

    last_key = None
    out = open(args.out, "a", buffering=1)

    while True:
        try:
            status = query(args.host, args.port, args.timeout)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as ex:
            print(f"[{now_iso()}] query failed: {ex}", file=sys.stderr)
            time.sleep(max(args.interval, 1.0))
            continue

        summary = summarize(status)
        key = change_key(summary)
        if key != last_key:
            print(f"[{now_iso()}] {format_line(summary)}", file=sys.stderr)
            out.write(json.dumps({
                "ts": now_iso(),
                "label": args.label,
                "host": args.host,
                "summary": summary,
                "status": status,
            }) + "\n")
            last_key = key

        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped.", file=sys.stderr)
