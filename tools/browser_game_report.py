"""Raportowanie testów prawdziwej gry. Moduł nie wymaga Playwright ani zasobów."""
from __future__ import annotations
import json
import math
import re
from pathlib import Path
from browser_mp_redact import sanitize_test_log

LABELS = {
    "preflight": "Pliki klienta, serwera i lokalne zasoby",
    "services": "Start serwera UDP, HTTP, katalogu i przekaźnika",
    "browser_run": "Pełny przebieg dwóch klientów WASM",
    "two_distinct_clients": "Dwa różne numery klientów",
    "both_spawned": "Obaj gracze pojawili się z bronią i zdrowiem",
    "both_move": "Ruch obu graczy i postęp migawek",
    "both_fire": "Strzały obu graczy zużywają amunicję",
    "damage_to_other_player": "Trafienia obniżają zdrowie przeciwnika",
    "target_died": "Śmierć przeciwnika",
    "target_respawned": "Odrodzenie po śmierci",
    "server_alive_after_disconnect": "Serwer działa po rozłączeniu gracza",
    "disconnected": "Potwierdzone rozłączenie klienta",
    "reconnected": "Ponowne połączenie klienta",
    "server_alive_after_reconnect": "Serwer działa po ponownym połączeniu",
    "assets": "Brak zgłoszonych brakujących modeli i materiałów",
    "runtime": "Brak awarii JavaScript, WASM i WebGL",
    "performance": "Płynność klatek zawierających rysowanie WebGL",
    "synchronization": "Postęp migawek i zgodność tożsamości graczy",
}
GAME_CHECKS = tuple(k for k in LABELS if k not in ("preflight", "services"))
ASSET_ERROR = re.compile(
    r"(?:couldn.t|could not|failed to|cannot|can.t|unable to).{0,90}"
    r"(?:load|find|register).{0,90}(?:material|model|xmodel|shader|image|texture)"
    r"|(?:material|xmodel|model|shader|image|texture).{0,90}(?:not found|missing|failed)"
    r"|\[cg-player\].*missing DObj", re.I)
FATAL_ERROR = re.compile(
    r"RuntimeError|unreachable|memory access out of bounds|GUARD CORRUPTED|"
    r"Aborted\(|SAFE_HEAP|WebGL.*CONTEXT_LOST|fatal error|ERR_DROP", re.I)

def log_findings(lines):
    """Błędy zasobów są oddzielone od zwykłych ostrzeżeń."""
    assets, fatal = [], []
    for line in lines:
        clean = sanitize_test_log(str(line)).strip()
        if ASSET_ERROR.search(clean):
            assets.append(clean[:1200])
        if FATAL_ERROR.search(clean):
            fatal.append(clean[:1200])
    return {"assets": list(dict.fromkeys(assets))[:100],
            "fatal": list(dict.fromkeys(fatal))[:100]}

def frame_metrics(intervals, min_fps=20.0, max_p95_ms=150.0, min_frames=30, max_frame_ms=1000.0):
    if len(intervals) < min_frames:
        return {"pass": False, "reason": "Za mało zmierzonych klatek z rysowaniem.",
                "frames": len(intervals)}
    if any(not isinstance(x, (int, float)) or not math.isfinite(x) or x <= 0 for x in intervals):
        return {"pass": False, "reason": "Nieprawidłowe czasy klatek."}
    ordered = sorted(intervals)
    fps = 1000 * len(intervals) / sum(intervals)
    p95 = ordered[max(0, math.ceil(.95 * len(ordered)) - 1)]
    return {"pass": fps >= min_fps and p95 <= max_p95_ms and max(intervals) <= max_frame_ms,
            "fps": round(fps, 2), "p95_ms": round(p95, 2),
            "max_ms": round(max(intervals), 2), "frames": len(intervals),
            "min_fps": min_fps, "max_p95_ms": max_p95_ms, "max_frame_ms": max_frame_ms}

def sync_metrics(samples, max_stall_ms=3000, max_gap_ms=1500, min_duration_ms=5000):
    """Próbki obu klientów z jednej fazy, co ~250 ms; nie porównuj zegarów różnych map."""
    problems = []
    if len(samples) < 3 or samples[-1]["t_ms"] - samples[0]["t_ms"] < min_duration_ms:
        return {"pass": False, "problems": ["Za krótka obserwacja synchronizacji."]}
    maximum_stall, maximum_gap, maximum_skew = 0, 0, 0
    for client in range(2):
        last = None
        advanced_at = samples[0]["t_ms"]
        progress = 0
        for row in samples:
            s = row["clients"][client]
            snap = s.get("snapshotTime")
            if (s.get("state") != 8 or not isinstance(snap, (int, float))
                    or not math.isfinite(snap) or s.get("client", -1) < 0
                    or s.get("client") != s.get("psClient")):
                problems.append("Nieaktywny klient lub niezgodna tożsamość: %d" % (client + 1))
                continue
            clock = s.get("serverTime")
            if not isinstance(clock, (int,float)) or not math.isfinite(clock):
                problems.append("Nieprawidłowy zegar klienta %d" % (client + 1))
            else:
                maximum_skew = max(maximum_skew, abs(clock-snap))
                if abs(clock-snap) > max_gap_ms:
                    problems.append("Zegar klienta odbiega od czasu migawki.")
            if last is not None:
                gap = snap - last
                maximum_gap = max(maximum_gap, gap)
                if gap < 0:
                    problems.append("Cofnięcie czasu migawki klienta %d" % (client + 1))
                if gap > 0:
                    maximum_stall = max(maximum_stall, row["t_ms"] - advanced_at)
                    advanced_at = row["t_ms"]
                    progress += 1
            maximum_stall = max(maximum_stall, row["t_ms"] - advanced_at)
            last = snap
        if not progress:
            problems.append("Brak nowych migawek klienta %d" % (client + 1))
    if maximum_stall > max_stall_ms:
        problems.append("Przekroczony limit zatrzymania migawek.")
    if maximum_gap > max_gap_ms:
        problems.append("Przekroczony limit skoku czasu migawki.")
    return {"pass": not problems, "problems": list(dict.fromkeys(problems)),
            "max_stall_ms": round(maximum_stall), "max_snapshot_gap_ms": maximum_gap,
            "max_clock_skew_ms": maximum_skew}

class Report:
    def __init__(self, output, metadata=None):
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.data = {"status": "FAIL", "metadata": metadata or {}, "checks": {}, "notes": []}
        for key in LABELS:
            self.data["checks"][key] = {"status": "NIE_WYKONANO", "name": LABELS[key], "detail": ""}
    def check(self, key, passed, detail=""):
        self.data["checks"][key] = {"status": "PASS" if passed else "FAIL",
                                   "name": LABELS.get(key, key), "detail": detail}
    def write(self):
        self.data["status"] = ("PASS" if all(v["status"] == "PASS" for v in self.data["checks"].values())
                               else "FAIL")
        text = json.dumps(self.data, ensure_ascii=False, indent=2, allow_nan=False)
        (self.output / "raport.json").write_text(sanitize_test_log(text), encoding="utf-8")
        rows = ["# Raport automatycznego testu CoD2", "",
                "**Wynik: %s**" % self.data["status"], "",
                "| Wynik | Sprawdzenie | Szczegóły |", "|---|---|---|"]
        for item in self.data["checks"].values():
            detail = str(item["detail"]).replace("|", "/").replace("\n", " ")
            rows.append("| %s | %s | %s |" % (item["status"], item["name"], detail))
        rows += ["", "## Warunki pomiaru", "", "```json",
                 json.dumps(self.data["metadata"], ensure_ascii=False, indent=2), "```",
                 "", "## Ograniczenia i uwagi", ""]
        rows += ["- " + note for note in self.data["notes"]]
        rows += ["", "Logi i dostępne zrzuty ekranu znajdują się w katalogu tego raportu.",
                 "NIE_WYKONANO nie jest wynikiem PASS. Brak zrzutu po zawieszeniu ma zostać jawnie opisany.", ""]
        (self.output / "raport.md").write_text(sanitize_test_log("\n".join(rows)), encoding="utf-8")
        return 0 if self.data["status"] == "PASS" else 1
