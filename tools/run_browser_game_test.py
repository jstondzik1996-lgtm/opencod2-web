#!/usr/bin/env python3
"""Izolowany test oryginalnego portu C/Emscripten; zawsze zapisuje raport PL."""
from __future__ import annotations
import argparse
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import signal
import socket
import subprocess
import sys
import time
import urllib.request
import zipfile
from browser_game_report import Report, log_findings
from browser_mp_redact import sanitize_test_log

TOOLS = Path(__file__).resolve().parent

def asset_path(root, value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError("Nieprawidłowa ścieżka zasobu.")
    rel = PurePosixPath(value)
    if rel.is_absolute() or ".." in rel.parts or ":" in value or "?" in value or "#" in value:
        raise ValueError("Zasób musi mieć bezpieczną ścieżkę względną.")
    # Dozwolone są istniejące lokalne junctions main/ do własnych zasobów.
    return root.joinpath(*rel.parts)

def preflight(web_root, server_exe, game_root):
    for name in ("cod2.html", "cod2.js", "cod2.wasm", "assets.json"):
        path = web_root / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError("Brak lub pusty plik klienta: " + name)
    with (web_root / "cod2.wasm").open("rb") as f:
        if f.read(8) != b"\0asm\x01\0\0\0":
            raise ValueError("Nieprawidłowy nagłówek WebAssembly.")
    with server_exe.open("rb") as f:
        if f.read(2) != b"MZ":
            raise ValueError("Nieprawidłowy plik serwera Win32.")
    if not (game_root / "main").is_dir() or not list((game_root / "main").glob("*.iwd")):
        raise ValueError("Brak lokalnych archiwów serwera w main/.")
    manifest = json.loads((web_root / "assets.json").read_text(encoding="utf-8"))
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Manifest zasobów jest pusty.")
    models = materials = total = 0
    seen = set()
    for entry in files:
        path = asset_path(web_root, entry["path"])
        if entry["path"] in seen:
            raise ValueError("Powtórzona ścieżka w manifeście.")
        seen.add(entry["path"])
        size = entry.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0 or path.stat().st_size != size:
            raise ValueError("Brak zasobu lub rozmiar niezgodny z manifestem: " + entry["path"])
        total += size
        if path.suffix.lower() == ".iwd":
            with zipfile.ZipFile(path) as archive:
                names = archive.namelist()
                models += sum(n.lower().startswith("xmodel/") for n in names)
                materials += sum(n.lower().startswith("materials/") for n in names)
    if not models or not materials:
        raise ValueError("Archiwa nie zawierają modeli xmodel/ lub materials/.")
    return {"archives_or_files": len(files), "bytes": total, "models": models,
            "materials": materials, "crc_checked": False}

def server_command(exe, game, home, port):
    # Nie zależymy od wyszukiwania luźnego cfg w fs_homepath. Starszy lokalny
    # build nie odnalazł qa-test.cfg mimo poprawnej inicjalizacji archiwów.
    values = {"dedicated":"1", "net_ip":"127.0.0.1", "net_port":str(port),
        "net_lanauthorize":"0", "fs_basepath":str(game), "fs_homepath":str(home),
        "sv_punkbuster":"0", "sv_pure":"0", "sv_maxclients":"4", "sv_allowDownload":"0",
        "g_gametype":"dm", "scr_dm_timelimit":"0", "scr_dm_scorelimit":"0",
        "logfile":"2", "g_debugBullets":"6", "g_debugDamage":"1"}
    command=[str(exe)]
    for key,value in values.items():
        command.extend(["+set",key,value])
    return command + ["+devmap","mp_toujane"]

def choose_ports():
    held = []
    try:
        for kind in (socket.SOCK_STREAM,) * 3 + (socket.SOCK_DGRAM,):
            s = socket.socket(socket.AF_INET, kind)
            held.append(s)
            if os.name == "nt":
                s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            s.bind(("127.0.0.1", 0))
        return [s.getsockname()[1] for s in held]
    finally:
        for s in held:
            s.close()

def udp_status(port, timeout=.5):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(b"\xff\xff\xff\xffgetstatus\n", ("127.0.0.1", port))
        data, peer = sock.recvfrom(65535)
    if peer != ("127.0.0.1", port) or not data.startswith(b"\xff\xff\xff\xffstatusResponse\n"):
        raise ValueError("Niepoprawna odpowiedź statusResponse serwera.")
    return data.decode("latin-1", errors="replace")

def http_get(url, limit=1_000_000):
    # Nie używaj systemowego proxy do połączeń loopback.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=2) as response:
        return response.read(limit), response.headers.get_content_type()

def start_owned(args, output, env, cwd):
    handle = output.open("wb")
    try:
        options = ({"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt"
                   else {"start_new_session": True})
        proc = subprocess.Popen(args, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT, **options)
        return proc, handle
    except BaseException:
        handle.close()
        raise

def stop_owned(proc, tree=False):
    if proc.poll() is not None:
        return
    tree_error = None
    if os.name == "nt":
        if tree:
            try:
                result = subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                if result.returncode and proc.poll() is None:
                    tree_error = "Nie udało się potwierdzić zamknięcia drzewa Chromium."
            except (OSError, subprocess.SubprocessError) as exc:
                tree_error = "Zamykanie drzewa Chromium: " + str(exc)
        if proc.poll() is None:
            proc.terminate()
    else:
        os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    if tree_error:
        raise RuntimeError(tree_error)

def wait_ready(processes, url, game_port, timeout=60):
    deadline = time.monotonic() + timeout
    error = ""
    while time.monotonic() < deadline:
        exited = [(Path(h.name).name, p.poll()) for p, h in processes if p.poll() is not None]
        if exited:
            raise RuntimeError("Proces usługi zakończył się podczas startu: " + str(exited))
        try:
            status = udp_status(game_port)
            if "\\mapname\\mp_toujane" not in status or "\\g_gametype\\dm" not in status:
                raise ValueError("Serwer nie uruchomił mp_toujane w trybie DM.")
            raw, _ = http_get(url + "/play/api/servers")
            servers = json.loads(raw).get("servers", [])
            if not any(s.get("online") and s.get("connect") == "127.0.0.1:%d" % game_port for s in servers):
                raise ValueError("Katalog nie potwierdza gotowego serwera.")
            wasm, mime = http_get(url + "/cod2.wasm", 8)
            if wasm != b"\0asm\x01\0\0\0" or mime != "application/wasm":
                raise ValueError("HTTP nie zwraca prawdziwego WASM z właściwym MIME.")
            # Przy --no-spawn HTTP ma osobny backend: sprawdź także gniazdo relay.
            return
        except (OSError, ValueError) as exc:
            error = str(exc)
            time.sleep(.25)
    raise TimeoutError("Limit startu usług: " + error)

def monitor_worker(owned, child, udp, args, health):
    started, last_udp = time.monotonic(), time.monotonic()
    while child[0].poll() is None:
        exited = [(Path(h.name).name if h else "usługa", p.poll())
                  for p, h in owned[:-1] if p.poll() is not None]
        if exited:
            raise RuntimeError("Proces usługi zakończył pracę podczas testu: " + str(exited))
        if time.monotonic() - started > args.timeout:
            raise TimeoutError("Watchdog: klient lub test nie zakończył się w limicie czasu.")
        try:
            udp_status(udp)
            last_udp = time.monotonic()
            health.append({"t": round(time.monotonic()-started, 3), "udp": True})
        except (OSError, ValueError):
            health.append({"t": round(time.monotonic()-started, 3), "udp": False})
            if time.monotonic()-last_udp > args.max_stall_ms / 1000:
                raise TimeoutError("Watchdog: serwer dedykowany przestał odpowiadać na UDP.")
        time.sleep(.5)

def run(args):
    # Unikalny katalog zapobiega zaliczeniu starego JSON jako nowego przebiegu.
    output = Path(args.output).resolve() / (
        time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3))
    report = Report(output, {"source_revision": args.source_revision,
                    "engine": "Istniejący CoD2 C/Emscripten",
                    "min_fps": args.min_fps, "max_p95_ms": args.max_p95_ms,
                    "timeout_seconds": args.timeout, "mode": "preflight" if args.preflight_only else "gra"})
    report.data["notes"] += [
        "PASS dotyczy wyłącznie tej mapy, scenariusza i podanych progów.",
        "Brak błędów modeli w logach nie potwierdza poprawnych kolorów ani widoczności każdego modelu.",
        "Pomiar klatek WebGL w headless Chromium nie zastępuje testu GPU użytkownika.",
        "Preflight sprawdza indeksy i rozmiary archiwów; nie sprawdza CRC całej zawartości.",
    ]
    owned, child, health = [], None, []
    stage = "preflight"
    try:
        print("Kontrola plików klienta, serwera i zasobów...", flush=True)
        web = Path(args.web_root).resolve()
        exe = Path(args.server_exe).resolve()
        game = Path(args.game_root).resolve()
        report.data["metadata"]["assets"] = preflight(web, exe, game)
        for module in ("playwright.sync_api", "websockets"):
            if importlib.util.find_spec(module) is None:
                raise ValueError("Brak pakietu Python: " + module)
        report.check("preflight", True, "Pliki i manifest poprawne; modele i materiały są w indeksach IWD.")
        if args.preflight_only:
            report.data["notes"].append("Wykonano wyłącznie kontrolę plików; gry, przeglądarki i sieci nie uruchamiano.")
            return report.write()
        stage = "services"
        http, master, relay, udp = choose_ports()
        base = "http://127.0.0.1:%d" % http
        report.data["metadata"]["url"] = base + "/cod2.html"
        report.data["metadata"]["server"] = "127.0.0.1:%d" % udp
        home = output / "server-home"
        (home / "main").mkdir(parents=True)
        (home / "main" / "qa-test.cfg").write_text(
            'set sv_punkbuster 0\nset sv_pure 0\nset sv_maxclients 4\nset sv_allowDownload 0\n'
            'set g_gametype dm\nset scr_dm_timelimit 0\nset scr_dm_scorelimit 0\n'
            'set logfile 2\nset g_debugBullets 6\nset g_debugDamage 1\ndevmap mp_toujane\n',
            encoding="ascii")
        directory = output / "servers.json"
        directory.write_text(json.dumps({"servers": [{"id": "qa-local", "name": "Test lokalny",
            "region": "local", "host": "127.0.0.1", "port": udp, "mode": "dm", "maxPlayers": 4}]}))
        env = os.environ.copy()
        env.update({"PYTHONUNBUFFERED": "1", "PORT": str(master), "RELAY_PORT": str(relay),
            "RELAY_HOST": "127.0.0.1", "COD2_SERVERS_FILE": str(directory),
            "COD2_SESSION_SECRET": secrets.token_urlsafe(32), "COD2_RELAY_ALLOW_ANY": "",
            "COD2_RELAY_MASTERLIST": "http://127.0.0.1:%d/masterlist/cod2/1.3" % master,
            "COD2_EXTERNAL_MASTER": "0", "COD2_ACCESS_KEY": "",
            "COD2_PORT_MIN": str(udp), "COD2_PORT_MAX": str(udp)})
        services = [
            ("server.log", server_command(exe, game, home, udp)),
            ("master.log", [sys.executable, str(TOOLS / "master_proxy" / "server.py")]),
            ("relay.log", [sys.executable, str(TOOLS / "game_proxy" / "relay.py")]),
            ("http.log", [sys.executable, str(TOOLS / "serve_web_local.py"), "--port", str(http),
                "--master-port", str(master), "--relay-port", str(relay),
                "--bind", "127.0.0.1", "--root", str(web), "--no-spawn"]),
        ]
        for name, command in services:
            owned.append(start_owned(command, output / name, env, output))
        print("Oczekiwanie na gotowość usług: " + base, flush=True)
        wait_ready(owned, base, udp)
        with socket.create_connection(("127.0.0.1", relay), timeout=2):
            pass
        report.check("services", True, "UDP odpowiada, katalog widzi serwer, HTTP udostępnia WASM, relay słucha.")
        stage = "browser_run"
        env.update({"COD2_TEST_URL": base + "/cod2.html", "COD2_TEST_SERVER": "127.0.0.1:%d" % udp,
            "COD2_TEST_OUTPUT": str(output), "COD2_TEST_NO_KEY": "1", "COD2_TEST_SESSION": "1",
            "COD2_TEST_MIN_FPS": str(args.min_fps), "COD2_TEST_MAX_P95_MS": str(args.max_p95_ms),
            "COD2_TEST_OBSERVE_SECONDS": str(args.observe_seconds),
            "COD2_TEST_MAX_STALL_MS": str(args.max_stall_ms), "COD2_TEST_MAX_GAP_MS": str(args.max_gap_ms)})
        if args.browser:
            env["COD2_TEST_BROWSER"] = args.browser
        child = start_owned([sys.executable, str(TOOLS / "browser_multiplayer_check.py")],
                            output / "browser-run.log", env, output)
        owned.append(child)
        print("Test dwóch klientów WASM; watchdog aktywny.", flush=True)
        monitor_worker(owned, child, udp, args, health)
        result_path = output / "multiplayer-result.json"
        if not result_path.exists():
            raise RuntimeError("Test przeglądarki zakończył się bez wyników; sprawdź worker-error.json i logi.")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        checks = result.get("checks", {})
        for key in report.data["checks"]:
            if key in checks:
                if not isinstance(checks[key], bool):
                    raise ValueError("Niepoprawny typ wyniku: " + key)
                report.check(key, checks[key], "Szczegóły: multiplayer-result.json")
        report.check("browser_run", child[0].returncode == 0,
                     "Kod zakończenia: %s" % child[0].returncode)
        observation = result.get("diagnostics", {}).get("observation", {})
        report.data["metadata"]["measurement"] = {k: observation.get(k) for k in
            ("performance", "synchronization", "renderer", "measurement", "duration_ms")}
    except KeyboardInterrupt:
        report.check(stage, False, "Test przerwany przez użytkownika.")
    except Exception as exc:
        report.check(stage, False, type(exc).__name__ + ": " + str(exc))
        report.data["notes"].append("Pełny test gry nie został potwierdzony. Zachowano dostępne logi.")
    finally:
        (output / "process-exits.json").write_text(json.dumps([
            {"log": Path(handle.name).name if isinstance(handle.name,str) else "usługa",
             "pid": proc.pid if isinstance(proc.pid,int) else None,
             "returncode_before_cleanup": proc.poll() if isinstance(proc.poll(),int) else None}
            for proc,handle in owned], ensure_ascii=False, indent=2), encoding="utf-8")
        for proc, handle in reversed(owned):
            try:
                stop_owned(proc, tree=bool(child and proc is child[0]))
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                report.check("cleanup", False, "Nie udało się zakończyć własnego procesu: " + str(exc))
            handle.close()
        (output / "server-health.json").write_text(json.dumps(health, indent=2), encoding="utf-8")
        lines = []
        for path in list(output.glob("*.log")) + list((output / "server-home").rglob("*.log")):
            clean = sanitize_test_log(path.read_text(encoding="utf-8", errors="replace"))
            path.write_text(clean, encoding="utf-8")
            lines += clean.splitlines()
        findings = log_findings(lines)
        if findings["assets"]:
            report.check("assets", False, "Braki zasobów w logach; patrz log-findings.json.")
        if findings["fatal"]:
            report.check("runtime", False, "Awaria silnika w logach; patrz log-findings.json.")
        (output / "log-findings.json").write_text(
            json.dumps(findings, ensure_ascii=False, indent=2), encoding="utf-8")
        if not list(output.glob("*.png")):
            report.data["notes"].append("Brak zrzutów ekranu: test nie dotarł do działającej przeglądarki lub klient się zawiesił.")
        code = report.write()
        print("Wynik: %s; raport: %s" % (report.data["status"], output / "raport.md"), flush=True)
    return code

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-root", required=True)
    parser.add_argument("--server-exe", required=True)
    parser.add_argument("--game-root", required=True)
    parser.add_argument("--output", default=str(TOOLS / ".runtime" / "game-qa"))
    parser.add_argument("--source-revision", default="nieustalona")
    parser.add_argument("--browser", default=os.environ.get("COD2_TEST_BROWSER"))
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--observe-seconds", type=float, default=12)
    parser.add_argument("--min-fps", type=float, default=20)
    parser.add_argument("--max-p95-ms", type=float, default=150)
    parser.add_argument("--max-stall-ms", type=float, default=3000)
    parser.add_argument("--max-gap-ms", type=float, default=1500)
    args = parser.parse_args()
    import math
    for key in ("timeout", "observe_seconds", "min_fps", "max_p95_ms", "max_stall_ms", "max_gap_ms"):
        if not math.isfinite(getattr(args, key)) or getattr(args, key) <= 0:
            parser.error("Progi muszą być dodatnimi, skończonymi liczbami.")
    if args.observe_seconds < 6:
        parser.error("Obserwacja musi trwać co najmniej 6 sekund.")
    return run(args)

if __name__ == "__main__":
    sys.exit(main())
