#!/usr/bin/env python3
"""Small operations on a connected badge: is it listening, what is it
running, put it in BadgeLink mode, send it back to the launcher.

This is glue, not machinery. Every hard part -- opening the rfc2217
console the way it wants to be opened, retrying a connect the proxy
refused, framing and CRC-checking records, probing and requesting
BadgeLink mode -- already exists in tools/testrun.py and is used by
`make cycle`. This just exposes three of those as commands you can run
on their own, which is what you want when a cycle has gone wrong and the
question is "is the app even alive?".

    make ping        does the app answer, and which build is it?
    make mode        put the badge in BadgeLink mode (probe first)
    make exitapp     ask a running app to return to the launcher

Exit codes: 0 fine, 1 nothing answered / could not switch.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from testrun import (  # noqa: E402
    connect,
    ensure_badgelink_mode,
    open_console,
    parse_record,
    read_line,
)

DEVNULL = open(os.devnull, "w")


def cmd_ping(args):
    """Connect and report the identity record the app volunteers."""
    port, ready = connect(args.port, args.timeout, DEVNULL)
    if port is None:
        print("ping: nothing answered -- the launcher is up, or the app is wedged")
        return 1
    print("ping: {app} answered".format(app=ready.get("app", "?")))
    for k in ("git", "built", "engine", "scene", "state"):
        if ready.get(k):
            print(f"  {k:<7} {ready[k]}")
    port.close()
    return 0


def cmd_mode(args):
    """Probe BadgeLink, and ask for it on the console if it is not there."""
    conn = args.badgelink_conn or (
        f"--tcp {args.badgelink}" if ":" in args.badgelink else f"--port {args.badgelink}"
    )
    if ensure_badgelink_mode(args.port, conn, DEVNULL):
        print("mode: BadgeLink is up")
        return 0
    print("mode: could not get BadgeLink up -- is an app still running?", file=sys.stderr)
    return 1


def cmd_exitapp(args):
    """Ask a running app to go back to the launcher, without a hard reset."""
    port, ready = connect(args.port, args.timeout, DEVNULL)
    if port is None:
        print("exit: nothing answered (probably already in the launcher)")
        return 0
    print("exit: {app} answered, sending EXIT".format(app=ready.get("app", "?")))
    buffer = bytearray()
    try:
        port.write(b"EXIT\n")
        port.flush()
        deadline = time.time() + 5
        while time.time() < deadline:
            line = read_line(port, buffer)
            if line is not None and parse_record(line)[0] == "BYE":
                print("exit: app said BYE")
                break
    except Exception:  # noqa: BLE001 - the link drops as the badge restarts
        pass
    try:
        port.close()
    except Exception:  # noqa: BLE001
        pass
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("ping", "mode", "exitapp"))
    ap.add_argument("--port", default=os.environ.get("PORT"))
    ap.add_argument("--badgelink", default=os.environ.get("BADGELINKPORT", ""))
    ap.add_argument("--badgelink-conn", default=None)
    ap.add_argument("--timeout", type=float, default=12)
    args = ap.parse_args()
    if not args.port:
        print("no --port and no $PORT", file=sys.stderr)
        return 1
    return {"ping": cmd_ping, "mode": cmd_mode, "exitapp": cmd_exitapp}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
