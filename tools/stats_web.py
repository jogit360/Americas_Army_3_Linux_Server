#!/usr/bin/env python3
"""Tiny stats web server for aa3_server's JSON export (doc sec 23).

Serves:
  /                   auto-refreshing player table
  /api/players.json   the raw export (aa3_server's StatsFile)

Stdlib only, read-only: it never writes the stats file, so it can run on
the game host or anywhere the file is synced to.

  stats_web.py --stats build/stats/players.json [--host 0.0.0.0 --port 8080]
"""
import argparse
import http.server
import pathlib

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AA3 Server Stats</title>
<style>
:root { --bg:#fff; --fg:#1d1d1f; --muted:#6e6e73; --line:#e5e5ea; --accent:#2f6f3e; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#151515; --fg:#f2f2f2; --muted:#9a9a9f; --line:#2c2c2e; --accent:#7cc48a; }
}
body { margin:0; padding:16px; background:var(--bg); color:var(--fg);
       font:14px/1.4 system-ui, sans-serif; }
h1 { font-size:20px; margin:0 0 4px; }
.meta { color:var(--muted); margin-bottom:16px; }
.wrap { overflow-x:auto; }
table { border-collapse:collapse; width:100%; min-width:720px; }
th, td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line);
         white-space:nowrap; }
th { color:var(--muted); font-weight:600; }
td.num, th.num { text-align:right; font-variant-numeric:tabular-nums; }
.on { color:var(--accent); font-weight:600; }
.off { color:var(--muted); }
</style></head><body>
<h1 id="title">AA3 Server Stats</h1>
<div class="meta" id="meta">loading…</div>
<div class="wrap"><table>
<thead><tr><th>Player</th><th>Status</th><th class="num">Time</th>
<th class="num">Team</th><th class="num">Score</th><th class="num">Neutralized</th>
<th class="num">Secured</th><th class="num">KIA</th><th class="num">Honor XP</th><th class="num">Shots</th><th class="num">Hits</th>
<th class="num">Kills</th><th class="num">Headshots</th><th class="num">Objectives</th>
<th class="num">Rounds won</th><th class="num">Distance (uu)</th><th class="num">Sprints</th></tr></thead>
<tbody id="rows"></tbody></table></div>
<script>
const sum = (o, suffix) => Object.entries(o || {})
  .filter(([k]) => k.endsWith(suffix)).reduce((a, [, v]) => a + Number(v), 0);
const hms = s => `${Math.floor(s/3600)}:${String(Math.floor(s/60)%60).padStart(2,"0")}:${String(s%60).padStart(2,"0")}`;
function cell(text, cls) { const td = document.createElement("td"); td.textContent = text;
  if (cls) td.className = cls; return td; }
async function refresh() {
  try {
    const d = await (await fetch("api/players.json", {cache: "no-store"})).json();
    document.getElementById("title").textContent = d.server || "AA3 Server Stats";
    document.getElementById("meta").textContent =
      `Map ${d.map} · updated ${new Date(d.generated_at * 1000).toLocaleTimeString()} · ` +
      `${d.players.filter(p => p.online).length} online`;
    const tb = document.getElementById("rows"); tb.replaceChildren();
    for (const p of d.players) {
      const r = p.round || {}, m = p.match || {}, s = p.scoreboard || {};
      const tr = document.createElement("tr");
      tr.append(cell(p.name || `client ${p.client_id}`),
        cell(p.online ? "online" : "left", p.online ? "on" : "off"),
        cell(hms(p.seconds_played), "num"), cell(p.team >= 0 ? p.team : "–", "num"),
        cell(s.score ?? 0, "num"), cell(s.neutralized ?? 0, "num"), cell(s.secured ?? 0, "num"),
        cell(s.kia ?? 0, "num"), cell(s.honor_xp ?? 0, "num"),
        cell(sum(r, "ShotsFired"), "num"), cell(sum(r, "ShotsHit"), "num"),
        cell(sum(r, "Kills"), "num"), cell(sum(r, "Headshots"), "num"),
        cell(s.objectives ?? r.iObjectivesCompleted ?? 0, "num"), cell(s.rounds_won ?? m.iRoundsWon ?? 0, "num"),
        cell(p.observed.distance_uu, "num"), cell(p.observed.sprints, "num"));
      tb.append(tr);
    }
  } catch (e) { document.getElementById("meta").textContent = "stats unavailable: " + e; }
}
refresh(); setInterval(refresh, 5000);
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", required=True, help="aa3_server StatsFile (JSON)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    stats = pathlib.Path(a.stats)

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path in ("/", "/index.html"):
                body, ctype = PAGE.encode(), "text/html; charset=utf-8"
            elif self.path.split("?")[0] == "/api/players.json":
                try:
                    body, ctype = stats.read_bytes(), "application/json"
                except OSError:
                    self.send_error(503, "stats file not available yet")
                    return
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    print(f"serving {stats} on http://{a.host}:{a.port}/")
    http.server.ThreadingHTTPServer((a.host, a.port), H).serve_forever()


if __name__ == "__main__":
    main()
