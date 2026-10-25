#!/usr/bin/env python3
"""Run several project dedicated-server instances and keep them alive.

Starts N copies of the dedicated server from one game root on consecutive UDP
ports, each with its own fs_homepath (logs/configs do not clash), bound to a
loopback/private address in the original LAN mode (net_lanauthorize 0): they
are meant to be reached only through the project relay. Writes the server
directory JSON that tools/master_proxy reads (COD2_SERVERS_FILE / --servers)
and restarts an instance that exits, with backoff.

Example (Windows, the clean server root from HANDOFF):
  python tools/run_server_fleet.py --exe work/native-server/cod2_win32_ded.exe ^
      --root work/native-server --config browser-test.cfg --count 2 ^
      --base-port 28961 --max-players 4 --servers-out tools/master_proxy/servers.local.json
Then:
  python tools/serve_web_local.py --port 8081 --master-port 20721 --relay-port 20722 ^
      --servers tools/master_proxy/servers.local.json --sessions

The game root must hold the player's own legally obtained files; this script
never copies or publishes them.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time


def _ipv4(text: str) -> bool:
    parts = text.split(".")
    return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)


def _private(ip: str) -> bool:
    a, b = (int(x) for x in ip.split(".")[:2])
    return a in (10, 127) or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)


class Instance:
    def __init__(self, index: int, args: argparse.Namespace) -> None:
        self.index = index
        self.port = args.base_port + index
        self.id = "%s-%d" % (args.id_prefix, index + 1)
        self.name = "%s #%d" % (args.name, index + 1)
        self.home = os.path.abspath(os.path.join(args.home_dir, self.id))
        self.args = args
        self.proc: subprocess.Popen | None = None
        self.restarts = 0
        self.started_at = 0.0
        self.log_path = os.path.join(self.home, "fleet.log")

    def command(self) -> list[str]:
        a = self.args
        cmd = list(a.exe_cmd) + [
            "+set", "dedicated", "1",
            "+set", "net_ip", a.bind,
            "+set", "net_port", str(self.port),
            "+set", "net_lanauthorize", "0",
            "+set", "fs_basepath", os.path.abspath(a.root),
            "+set", "fs_homepath", self.home,
            "+set", "sv_maxclients", str(a.max_players),
            "+set", "sv_hostname", self.name,
        ]
        for extra in a.set or []:
            name, _, value = extra.partition("=")
            cmd += ["+set", name, value]
        if a.config:
            cmd += ["+exec", a.config]
        return cmd

    def start(self) -> None:
        os.makedirs(self.home, exist_ok=True)
        log = open(self.log_path, "ab", buffering=0)
        log.write(("\n=== start %s port %d (restart %d) ===\n" % (
            time.strftime("%Y-%m-%d %H:%M:%S"), self.port, self.restarts)).encode())
        kwargs = {}
        if os.name == "nt":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        self.proc = subprocess.Popen(self.command(), cwd=os.path.abspath(self.args.root),
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                     **kwargs)
        log.close()
        self.started_at = time.time()
        print("[fleet] %s started pid %d on %s:%d" % (self.id, self.proc.pid, self.args.bind, self.port),
              flush=True)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def directory_entry(self) -> dict:
        a = self.args
        return {"id": self.id, "name": self.name, "region": a.region, "host": a.bind,
                "port": self.port, "mode": a.mode, "maxPlayers": a.max_players}


def write_directory(path: str, instances: list[Instance]) -> None:
    data = {"servers": [i.directory_entry() for i in instances]}
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
    os.replace(tmp, path)


def supervise(instances: list[Instance], args: argparse.Namespace, stop: threading.Event) -> None:
    for inst in instances:
        inst.start()
    while not stop.wait(1.0):
        for inst in instances:
            code = inst.proc.poll() if inst.proc else 0
            if code is None:
                continue
            uptime = time.time() - inst.started_at
            # Backoff: crash loops wait up to 30 s; a long healthy run resets it.
            if uptime > 60:
                inst.restarts = 0
            delay = min(30.0, 1.0 * (2 ** min(inst.restarts, 5)))
            print("[fleet] %s exited with %s after %.0fs; restarting in %.0fs" % (
                inst.id, code, uptime, delay), flush=True)
            if args.max_restarts >= 0 and inst.restarts >= args.max_restarts:
                print("[fleet] %s reached --max-restarts; leaving it stopped" % inst.id, flush=True)
                inst.proc = None
                inst.started_at = float("inf")
                continue
            if stop.wait(delay):
                break
            inst.restarts += 1
            inst.start()
    for inst in instances:
        inst.stop()


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run and supervise project dedicated-server instances")
    p.add_argument("--exe", required=True,
                   help="dedicated server executable (or a command line, split on spaces with --exe-split)")
    p.add_argument("--exe-split", action="store_true", help="treat --exe as a command line (tests)")
    p.add_argument("--root", required=True, help="game root holding main/ (fs_basepath and cwd)")
    p.add_argument("--config", default="", help="config to +exec after the fleet settings")
    p.add_argument("--count", type=int, default=2)
    p.add_argument("--base-port", type=int, default=28961)
    p.add_argument("--bind", default="127.0.0.1", help="net_ip; loopback/private only")
    p.add_argument("--max-players", type=int, default=16)
    p.add_argument("--mode", default="dm")
    p.add_argument("--region", default="local")
    p.add_argument("--name", default="WWII Browser MP")
    p.add_argument("--id-prefix", default="srv")
    p.add_argument("--home-dir", default=os.path.join("work", "fleet"))
    p.add_argument("--servers-out", required=True, help="directory JSON for the master proxy")
    p.add_argument("--set", action="append", metavar="DVAR=VALUE", help="extra +set (repeatable)")
    p.add_argument("--max-restarts", type=int, default=-1, help="-1 = unlimited")
    args = p.parse_args(argv)
    if not _ipv4(args.bind) or not _private(args.bind):
        p.error("--bind must be a loopback/private IPv4 address; servers are reached via the relay")
    if not 1 <= args.count <= 32:
        p.error("--count must be 1-32")
    if not 1 <= args.max_players <= 64:
        p.error("--max-players must be 1-64")
    if not 1024 <= args.base_port <= 65535 - args.count:
        p.error("--base-port out of range")
    for extra in args.set or []:
        name = extra.partition("=")[0]
        if not name or not all(c.isalnum() or c == "_" for c in name):
            p.error("bad --set %r" % extra)
    args.exe_cmd = args.exe.split() if args.exe_split else [args.exe]
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    instances = [Instance(i, args) for i in range(args.count)]
    write_directory(args.servers_out, instances)
    print("[fleet] directory written to %s (%d servers)" % (args.servers_out, len(instances)), flush=True)
    stop = threading.Event()

    def on_signal(*_):
        stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, on_signal)
        except (ValueError, OSError):
            pass
    try:
        supervise(instances, args, stop)
    except KeyboardInterrupt:
        stop.set()
        for inst in instances:
            inst.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
