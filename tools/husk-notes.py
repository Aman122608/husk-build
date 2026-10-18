#!/usr/bin/env python3
"""Print every user note carried inside Husk logs.

A Husk log can open with the user's own description of what they saw:

    === MESSAGE FROM THE USER ===
    Doodle jump, buttons were all over the place and didn't leave ...
    === END MESSAGE ===

That note is the fastest route to a cause -- it says what the user actually
experienced, which the log itself may only imply -- and it sits in the first
three lines, exactly where a body-first skim misses it. Read the notes BEFORE
the body, every time.

    python tools/husk-notes.py                 # every log, oldest first
    python tools/husk-notes.py --newest 5      # the five most recent
    python tools/husk-notes.py --dir <path>    # a different log directory
"""
import argparse
import glob
import os
import re
import sys

# logs live beside the repo: <Desktop>/HuskLogs, and this file is <repo>/tools/husk-notes.py
DEFAULT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..", "HuskLogs"))
NOTE = re.compile(r"===\s*MESSAGE FROM THE USER\s*===\s*\n(.*?)\n\s*===\s*END MESSAGE\s*===", re.S)


def logs_in(directory):
    found = glob.glob(os.path.join(directory, "*.log"))
    return sorted(found, key=os.path.getmtime)


def notes_of(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    return [m.group(1).strip() for m in NOTE.finditer(text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=None, help="log directory (default: %s)" % DEFAULT_DIR)
    ap.add_argument("--newest", type=int, default=0, help="only the N most recent logs (0 = all)")
    ap.add_argument("--all-logs", action="store_true", help="also list logs that carry no note")
    args = ap.parse_args()

    directory = args.dir or DEFAULT_DIR
    if not os.path.isdir(directory):
        sys.exit("no such log directory: %s" % directory)

    logs = logs_in(directory)
    if args.newest:
        logs = logs[-args.newest:]

    shown = 0
    silent = []
    for path in logs:
        ns = notes_of(path)
        if not ns:
            silent.append(os.path.basename(path))
            continue
        for n in ns:
            shown += 1
            print("%s  (%s)" % (os.path.basename(path), "{:,} bytes".format(os.path.getsize(path))))
            for line in n.splitlines():
                print("    | " + line.strip())
            print()
    print("%d note(s) in %d log(s)" % (shown, len(logs)))
    if args.all_logs and silent:
        print("logs without a note (%d): %s" % (len(silent), ", ".join(silent)))


if __name__ == "__main__":
    main()
