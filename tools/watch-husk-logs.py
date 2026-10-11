#!/usr/bin/env python3
"""
Watch the Husk log directory and announce each new log, then exit.

One line per new .log file, which is what a Hermes background-process notification
pattern matches on -- so a log arriving from the phone turns into a turn on the PC
without anyone polling for it.

**It exits as soon as it has announced something, and that is deliberate.** A
pattern-based notification is capped at 8 deliveries per process lifetime, after
which the pattern is switched off and the process falls back to notifying only on
exit -- so a watcher that runs forever goes silent exactly when it has been useful
8 times. Exiting after each announcement means every notification arrives through
the on-exit path, which has no cap. Restart it after handling a log.

**The state file is what makes exiting safe.** If the watcher is down when a log
lands, a fresh start would otherwise treat that file as already known and never
announce it. Instead the announced set is persisted, so a restart announces
anything it missed (one line each) and exits again -- a backlog drains one
notification at a time rather than being silently dropped.

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
    """Print each new log, persist the set, and exit so the notification is uncapped."""
    for name in sorted(names):
        print(f"NEW HUSK LOG: {name} ({present.get(name, 0):,} bytes)", flush=True)
    announced.update(names)
    save_announced(announced)
    sys.exit(0)


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
