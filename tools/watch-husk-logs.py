#!/usr/bin/env python3
"""
Watch the Husk log directory and announce each new log. Stays running.

One line per new .log file, which is what a Hermes background-process notification
pattern matches on -- so a log arriving from the phone turns into a turn on the PC
without anyone polling for it.

**It stays running, and that is a correction.** It used to exit after each announcement so that
every notification arrived through the on-exit path, which has no cap. In practice that meant the
watcher had to be re-armed by hand after every single log, and that was missed twice in one
session -- the user's next run landed silently and they had to report that the listener was dead.
A watcher that stops watching is not worth the cap it buys. So: it keeps polling, announces each
new log once, and flushes the line immediately (a long-running process's stdout is block-buffered,
so without a flush the announcement would never reach the notification path at all).

Announcements are one line per log and logs arrive one per run, minutes apart, so the pattern
watch is never over-fired and does not auto-disable.

**The state file is what makes exiting safe.** If the watcher is down when a log
lands, a fresh start would otherwise treat that file as already known and never
announce it. Instead the announced set is persisted, so a restart announces
anything it missed (one line each) and keeps watching -- a backlog drains through
the same path as a fresh arrival.

Deliberately separate from husk_log_receiver.py: the receiver's job is to accept a
POST and write a file, and a watcher that also catches a log copied in by hand,
re-fetched, or written by an older receiver build is strictly more useful than one
bolted to the HTTP path. It also means the receiver can be restarted without losing
the notification.

Only *new* files are announced, never growth: the receiver writes one timestamped
file per log, so a growing file is just the current upload and announcing it would
fire repeatedly.
"""
import os
import sys
import time

LOG_DIR = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Aiden\Desktop\HuskLogs"
POLL = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
STATE = os.path.join(LOG_DIR, ".announced")


def logs():
    """{name: size} for every .log in the directory, tolerating races."""
    out = {}
    try:
        for name in os.listdir(LOG_DIR):
            if not name.endswith(".log"):
                continue
            try:
                out[name] = os.stat(os.path.join(LOG_DIR, name)).st_size
            except OSError:
                continue          # removed between listdir and stat
    except OSError as e:
        print(f"watch: cannot read {LOG_DIR}: {e}", flush=True)
    return out


def load_announced():
    try:
        with open(STATE, encoding="utf-8") as fh:
            return set(fh.read().split("\n")) - {""}
    except OSError:
        return None           # no state file yet: first run


def save_announced(names):
    tmp = STATE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write("\n".join(sorted(names)))
    os.replace(tmp, STATE)    # atomic, so a kill mid-write cannot lose the set


announced = load_announced()
present = logs()

if announced is None:
    # First run: baseline on what is already there. Announcing the whole backlog
    # would be 13 lines of history nobody asked for.
    save_announced(set(present))
    announced = set(present)
    print(f"watching {LOG_DIR} -- baselined on {len(present)} existing log(s), "
          f"polling every {POLL:g}s", flush=True)
else:
    print(f"watching {LOG_DIR} -- {len(announced)} announced, {len(present)} present, "
          f"polling every {POLL:g}s", flush=True)


def announce(names):
    """Print each new log, persist the set, and keep watching."""
    for name in sorted(names):
        print(f"NEW HUSK LOG: {name} ({present.get(name, 0):,} bytes)", flush=True)
    announced.update(names)
    save_announced(announced)
    sys.stdout.flush()          # the notification path reads this as it is written


# Drain anything that landed while we were not running, before settling in to poll.
missed = set(present) - announced
if missed:
    announce(missed)

while True:
    time.sleep(POLL)
    present = logs()
    new = set(present) - announced
    if new:
        announce(new)
