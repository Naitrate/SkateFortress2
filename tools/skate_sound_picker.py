#!/usr/bin/env python3
"""Local page for assigning extracted Skate 3 sounds to skate events.

    python3 tools/skate_sound_picker.py        # then open http://127.0.0.1:8770

Lists every WAV under game/mod_tf/sound/skate/banks (from
the in-game Skate 3 setup). Click a sound to hear it; add it to the
selected event. Every change rewrites sound/skate/events.txt, which the game
reads (run `skate_reload_sounds` in the console to pick changes up live).
Serves 127.0.0.1 only; nothing leaves this machine.
"""
import html
import json
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
SOUND = ROOT / "game/mod_tf/sound"
BANKS = SOUND / "skate/banks"
EVENTS_TXT = SOUND / "skate/events.txt"
EVENTS_JSON = SOUND / "skate/events.json"
PORT = 8770

# name, kind, what to listen for, banks most likely to hold it
EVENTS = [
    ("pop", "one-shot", "Ollie pop: sharp tail snap as the board leaves the ground", "Skate_Collisions"),
    ("land", "one-shot", "Normal landing: wheels and deck hitting the ground", "Skate_Collisions"),
    ("land_hard", "one-shot", "Big landing from height: heavier slam", "Skate_Collisions"),
    ("bail", "one-shot", "Body slam when the skater wipes out", "Skate_Collisions, Bodyslide"),
    ("impact", "one-shot", "Board knocking into a wall or ledge while rolling", "Skate_Collisions, Skate_Metal"),
    ("flip", "one-shot", "Board flip / spin whoosh in the air", "Sk8_Air_Flip_Tricks, Sk82_Whsh_Bys"),
    ("board_step", "one-shot", "Stepping off or onto the board", "Skate_Collisions, fstep_skateshoe1_sm"),
    ("grind_start", "one-shot", "Trucks or deck hitting a rail/ledge to start a grind", "GRINDS, Skate_Metal"),
    ("grind", "loop", "Grind/slide loop while on a rail or ledge (first entry is used)", "GRINDS"),
    ("skid", "loop", "Powerslide wheel skid loop (first entry is used)", "WHEEL_SKID_BANK"),
]


def load_mapping() -> dict:
    if EVENTS_JSON.is_file():
        return json.loads(EVENTS_JSON.read_text())
    return {name: [] for name, *_ in EVENTS}


def save_mapping(mapping: dict) -> None:
    EVENTS_JSON.write_text(json.dumps(mapping, indent=1))
    lines = ['"SkateSounds"', "{"]
    for name, *_ in EVENTS:
        lines.append(f'\t"{name}"')
        lines.append("\t{")
        for wave_path in mapping.get(name, []):
            lines.append(f'\t\t"wave"\t"{wave_path}"')
        lines.append("\t}")
    lines.append("}")
    EVENTS_TXT.write_text("\n".join(lines) + "\n")


def bank_listing() -> dict:
    banks = {}
    for folder in sorted(p for p in BANKS.iterdir() if p.is_dir()):
        sounds = []
        for path in sorted(folder.glob("*.wav")):
            try:
                with wave.open(str(path)) as w:
                    duration = w.getnframes() / w.getframerate()
            except (wave.Error, EOFError):
                continue
            sounds.append({"path": f"skate/banks/{folder.name}/{path.name}", "n": path.stem, "dur": round(duration, 2)})
        if sounds:
            banks[folder.name] = sounds
    return banks


PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Skate Sound Picker</title>
<style>
:root { --bg:#f6f5f2; --panel:#fff; --ink:#1d1d1b; --muted:#6b6a66; --line:#dddad3; --accent:#c2410c; --accent-soft:#fde8dc; --wave:#8a8780; --playing:#0f766e; }
@media (prefers-color-scheme: dark) { :root { --bg:#171716; --panel:#211f1d; --ink:#ecebe7; --muted:#9c9a94; --line:#34322f; --accent:#fb923c; --accent-soft:#3b2416; --wave:#7c7a74; --playing:#2dd4bf; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:14px/1.45 system-ui, sans-serif; }
header { padding:14px 20px; border-bottom:1px solid var(--line); display:flex; gap:16px; align-items:baseline; flex-wrap:wrap; }
header h1 { font-size:17px; margin:0; }
header p { margin:0; color:var(--muted); }
main { display:grid; grid-template-columns:320px 1fr; min-height:calc(100vh - 56px); }
aside { border-right:1px solid var(--line); padding:12px; overflow:auto; max-height:calc(100vh - 56px); position:sticky; top:0; }
.event { border:1px solid var(--line); border-radius:8px; padding:8px 10px; margin-bottom:8px; background:var(--panel); cursor:pointer; }
.event.active { border-color:var(--accent); background:var(--accent-soft); }
.event b { font-family:ui-monospace, monospace; }
.event .kind { color:var(--muted); font-size:12px; margin-left:6px; }
.event .hint { color:var(--muted); font-size:12px; margin:2px 0 6px; }
.chips { display:flex; flex-wrap:wrap; gap:4px; }
.chip { font:12px ui-monospace, monospace; background:var(--bg); border:1px solid var(--line); border-radius:999px; padding:1px 4px 1px 8px; display:inline-flex; gap:4px; align-items:center; }
.chip button { border:0; background:none; color:var(--muted); cursor:pointer; padding:0 4px; font-size:13px; }
.chip span { cursor:pointer; }
section { padding:12px 16px; overflow:auto; }
.controls { display:flex; gap:12px; align-items:center; flex-wrap:wrap; margin-bottom:12px; }
.tabs { display:flex; gap:6px; flex-wrap:wrap; }
.tab { border:1px solid var(--line); background:var(--panel); color:var(--ink); border-radius:6px; padding:4px 9px; cursor:pointer; font-size:13px; }
.tab.active { border-color:var(--accent); color:var(--accent); }
.grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(150px, 1fr)); gap:8px; }
.tile { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:6px 8px; cursor:pointer; }
.tile.current { outline:2px solid var(--playing); }
.tile .row { display:flex; justify-content:space-between; font:12px ui-monospace, monospace; color:var(--muted); }
.tile .row b { color:var(--ink); }
.tile canvas { width:100%; height:34px; display:block; margin:4px 0; }
.tile .add { width:100%; border:1px solid var(--line); background:var(--bg); color:var(--ink); border-radius:5px; font-size:12px; cursor:pointer; padding:2px; }
.tile .used { color:var(--accent); font-size:11px; }
kbd { font:11px ui-monospace, monospace; border:1px solid var(--line); border-radius:3px; padding:0 4px; background:var(--panel); }
@media (max-width: 760px) { main { grid-template-columns:1fr; } aside { position:static; max-height:none; border-right:0; border-bottom:1px solid var(--line); } }
</style></head><body>
<header><h1>Skate Sound Picker</h1>
<p>Pick an event on the left, click sounds to hear them, <b>Add</b> the right ones. Saves to <code>events.txt</code> instantly; in game run <code>skate_reload_sounds</code>.
Keys: <kbd>&larr;</kbd><kbd>&rarr;</kbd> previous/next and play, <kbd>Space</kbd> replay, <kbd>Enter</kbd> add.</p></header>
<main><aside id="events"></aside>
<section><div class="controls"><div class="tabs" id="tabs"></div>
<label>Max length <input type="range" id="maxdur" min="0.1" max="3" step="0.05" value="3"> <span id="maxdurv">3.0 s</span></label></div>
<div class="grid" id="grid"></div></section></main>
<script>
const EVENTS = __EVENTS__;
let banks = {}, mapping = {}, activeEvent = EVENTS[0][0], activeBank = null, current = -1, visible = [];
const audio = new Audio();
const ctx = new (window.AudioContext || window.webkitAudioContext)();
async function api(path, body) {
  const r = await fetch(path, body ? {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)} : {});
  return r.json();
}
function play(path) { audio.src = '/sound/' + path; audio.currentTime = 0; audio.play(); }
function renderEvents() {
  const el = document.getElementById('events'); el.innerHTML = '';
  for (const [name, kind, hint, where] of EVENTS) {
    const d = document.createElement('div'); d.className = 'event' + (name === activeEvent ? ' active' : '');
    d.innerHTML = `<b>${name}</b><span class="kind">${kind}</span><div class="hint">${hint}<br>Try: ${where}</div><div class="chips"></div>`;
    d.onclick = () => { activeEvent = name; renderEvents(); renderGrid(); };
    const chips = d.querySelector('.chips');
    for (const p of (mapping[name] || [])) {
      const c = document.createElement('span'); c.className = 'chip';
      const label = document.createElement('span'); label.textContent = p.split('/').slice(-2).join('/').replace('.wav','');
      label.onclick = (e) => { e.stopPropagation(); play(p); };
      const x = document.createElement('button'); x.textContent = '×'; x.title = 'Remove';
      x.onclick = async (e) => { e.stopPropagation(); mapping[name] = mapping[name].filter(q => q !== p); mapping = await api('/api/mapping', mapping); renderEvents(); renderGrid(); };
      c.append(label, x); chips.append(c);
    }
    el.append(d);
  }
}
function renderTabs() {
  const el = document.getElementById('tabs'); el.innerHTML = '';
  for (const b of Object.keys(banks)) {
    const t = document.createElement('button'); t.className = 'tab' + (b === activeBank ? ' active' : '');
    t.textContent = `${b} (${banks[b].length})`;
    t.onclick = () => { activeBank = b; current = -1; renderTabs(); renderGrid(); };
    el.append(t);
  }
}
const observer = new IntersectionObserver(entries => {
  for (const e of entries) if (e.isIntersecting) { observer.unobserve(e.target); drawWave(e.target); }
});
async function drawWave(canvas) {
  try {
    const buf = await (await fetch('/sound/' + canvas.dataset.path)).arrayBuffer();
    const data = (await ctx.decodeAudioData(buf)).getChannelData(0);
    const w = canvas.width = canvas.clientWidth * devicePixelRatio, h = canvas.height = canvas.clientHeight * devicePixelRatio;
    const g = canvas.getContext('2d'); g.fillStyle = getComputedStyle(document.body).getPropertyValue('--wave');
    const step = Math.max(1, Math.floor(data.length / w));
    for (let x = 0; x < w; x++) { let m = 0; for (let i = x * step; i < (x + 1) * step && i < data.length; i++) m = Math.max(m, Math.abs(data[i])); const bar = Math.max(1, m * h); g.fillRect(x, (h - bar) / 2, 1, bar); }
  } catch (err) {}
}
function usedBy(path) { return Object.entries(mapping).filter(([, v]) => v.includes(path)).map(([k]) => k); }
function renderGrid() {
  const max = parseFloat(document.getElementById('maxdur').value);
  document.getElementById('maxdurv').textContent = max.toFixed(2) + ' s';
  const grid = document.getElementById('grid'); grid.innerHTML = '';
  visible = (banks[activeBank] || []).filter(s => s.dur <= max);
  visible.forEach((s, i) => {
    const t = document.createElement('div'); t.className = 'tile' + (i === current ? ' current' : '');
    const used = usedBy(s.path);
    t.innerHTML = `<div class="row"><b>#${s.n}</b><span>${s.dur.toFixed(2)} s</span></div><canvas data-path="${s.path}"></canvas>` +
      `<button class="add">Add to ${activeEvent}</button>${used.length ? `<div class="used">${used.join(', ')}</div>` : ''}`;
    t.onclick = () => select(i);
    t.querySelector('.add').onclick = (e) => { e.stopPropagation(); select(i, false); add(); };
    grid.append(t); observer.observe(t.querySelector('canvas'));
  });
}
function select(i, sound = true) {
  if (!visible.length) return;
  current = Math.max(0, Math.min(visible.length - 1, i));
  document.querySelectorAll('.tile').forEach((t, j) => t.classList.toggle('current', j === current));
  document.querySelectorAll('.tile')[current]?.scrollIntoView({block:'nearest'});
  if (sound) play(visible[current].path);
}
async function add() {
  if (current < 0) return;
  const p = visible[current].path; mapping[activeEvent] = mapping[activeEvent] || [];
  if (!mapping[activeEvent].includes(p)) mapping[activeEvent].push(p);
  mapping = await api('/api/mapping', mapping); renderEvents(); renderGrid(); select(current, false);
}
document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key === 'ArrowRight') { e.preventDefault(); select(current + 1); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); select(current - 1); }
  else if (e.key === ' ') { e.preventDefault(); if (current >= 0) play(visible[current].path); }
  else if (e.key === 'Enter') { e.preventDefault(); add(); }
});
document.getElementById('maxdur').oninput = renderGrid;
(async () => { banks = await api('/api/banks'); mapping = await api('/api/mapping'); activeBank = Object.keys(banks).includes('skate_collisions') ? 'skate_collisions' : Object.keys(banks)[0]; renderEvents(); renderTabs(); renderGrid(); })();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, kind):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            page = PAGE.replace("__EVENTS__", json.dumps(EVENTS))
            self.send(200, page.encode(), "text/html; charset=utf-8")
        elif self.path == "/api/banks":
            self.send(200, json.dumps(bank_listing()).encode(), "application/json")
        elif self.path == "/api/mapping":
            self.send(200, json.dumps(load_mapping()).encode(), "application/json")
        elif self.path.startswith("/sound/"):
            target = (SOUND / unquote(self.path[len("/sound/"):])).resolve()
            if not target.is_relative_to(SOUND.resolve()) or target.suffix != ".wav" or not target.is_file():
                self.send(404, b"not found", "text/plain")
                return
            self.send(200, target.read_bytes(), "audio/wav")
        else:
            self.send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path != "/api/mapping":
            self.send(404, b"not found", "text/plain")
            return
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        known = {name for name, *_ in EVENTS}
        mapping = {
            name: [p for p in body.get(name, []) if isinstance(p, str) and p.startswith("skate/banks/") and (SOUND / p).is_file()]
            for name in known
        }
        save_mapping(mapping)
        self.send(200, json.dumps(mapping).encode(), "application/json")


if __name__ == "__main__":
    if not BANKS.is_dir():
        raise SystemExit("Run the in-game Skate 3 setup (skate_setup) first.")
    if not EVENTS_TXT.is_file():
        save_mapping(load_mapping())
    print(f"Skate Sound Picker on http://127.0.0.1:{PORT} (writes {html.escape(str(EVENTS_TXT))})")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
