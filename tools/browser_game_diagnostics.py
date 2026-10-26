"""Obserwacja przeglądarki bez zmian kodu renderera ani obrażeń."""
import time
from browser_game_report import frame_metrics, sync_metrics, log_findings

# Liczymy tylko takty rAF, w których od poprzedniej próbki nastąpiło wywołanie
# draw*. To przybliżenie prezentowanych klatek, nie czas GPU ani sam licznik rAF.
FRAME_PROBE = """
(() => {
  document.addEventListener('webglcontextlost', () => console.error('WebGL CONTEXT_LOST'), true);
  let draws = 0, priorDraws = 0, last = null;
  window.__cod2Probe = {enabled:false, intervals:[], draws:0};
  for (const type of [window.WebGLRenderingContext, window.WebGL2RenderingContext]) {
    if (!type) continue;
    for (const name of ['drawArrays','drawElements','drawArraysInstanced','drawElementsInstanced']) {
      const original = type.prototype[name];
      if (!original) continue;
      type.prototype[name] = function(...args) {
        const result = original.apply(this, args); draws++; return result;
      };
    }
  }
  function tick(t) {
    const p = window.__cod2Probe;
    if (!p.enabled) { last = null; priorDraws = draws; }
    else if (draws > priorDraws) {
      if (last !== null && p.intervals.length < 20000) p.intervals.push(t-last);
      last = t; p.draws += draws-priorDraws; priorDraws = draws;
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
})();
"""

class Diagnostics:
    def __init__(self):
        self.lines, self.errors, self.network, self.samples = [], [], [], []
    def event(self, kind, value):
        if kind.startswith("console"):
            self.lines.append(value.get("text", ""))
        elif kind.startswith("error") or kind.startswith("crash"):
            self.errors.append(str(value))
    def attach(self, page, index, emit):
        page.on("crash", lambda: emit("crash%d" % index, "Proces renderujący klienta uległ awarii."))
        page.on("response", lambda response: self.network.append(
            {"status": response.status, "url": response.url})
            if response.status >= 400 and any(x in response.url.split("?")[0]
                for x in (".wasm", ".js", ".iwd", "assets.json")) else None)
        page.on("requestfailed", lambda request: self.network.append(
            {"failure": request.failure, "url": request.url})
            if request.resource_type in ("document", "script", "fetch", "xhr", "websocket") else None)
    def observe(self, pages, state, emit, seconds, min_fps, max_p95_ms, max_stall_ms, max_gap_ms):
        pages[0].bring_to_front()
        # Krótkie rozgrzanie po wczytaniu mapy nie wchodzi do pomiaru.
        pages[0].wait_for_timeout(2000)
        pages[0].evaluate("() => {const p=window.__cod2Probe; p.intervals=[]; p.draws=0; p.enabled=true;}")
        started = time.monotonic()
        while time.monotonic() - started < seconds:
            row = {"t_ms": (time.monotonic()-started)*1000,
                   "clients": [state(page) for page in pages]}
            self.samples.append(row)
            emit("observation", row)
            pages[0].wait_for_timeout(250)
        # Do pomiaru włącz też końcowy zastój; inaczej zamrożenie po kilku
        # poprawnych klatkach mogłoby dać fałszywy PASS.
        frames = pages[0].evaluate("() => {const p=window.__cod2Probe; p.enabled=false; return p;}")
        intervals = frames["intervals"]
        duration = (time.monotonic()-started)*1000
        accounted = sum(intervals)
        trailing = max(0, duration-accounted)
        if trailing > 250:
            intervals.append(trailing)
        performance = frame_metrics(intervals, min_fps, max_p95_ms)
        sync = sync_metrics(self.samples, max_stall_ms, max_gap_ms)
        renderer = pages[0].evaluate("""() => {
          const canvas = document.querySelector('canvas');
          const gl = canvas && canvas.getContext('webgl2');
          if (!gl) return {available:false};
          const ext = gl.getExtension('WEBGL_debug_renderer_info');
          return {available:true, renderer:ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : 'nieujawniony'};
        }""")
        result = {"renderer": renderer, "performance": performance, "synchronization": sync,
                  "samples": self.samples, "frame_intervals_ms": intervals,
                  "measurement": "Takty rAF z wywołaniami draw WebGL; główny klient na pierwszym planie.",
                  "duration_ms": duration}
        emit("diagnostics", result)
        return result
    def findings(self):
        found = log_findings(self.lines)
        found["page_errors"] = self.errors
        found["network_errors"] = self.network
        return found
