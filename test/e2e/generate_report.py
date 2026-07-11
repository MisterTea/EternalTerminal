#!/usr/bin/env python3
"""Generate a self-contained HTML report with inline SVG charts from E2E JSONL data.

Usage: python3 generate_report.py <results_dir> <output.html>
"""
import json
import sys
from datetime import datetime
from pathlib import Path


def load_scenario(results_dir, name):
    jsonl = Path(results_dir) / f"{name}.jsonl"
    meta_file = Path(results_dir) / f"{name}_meta.json"
    if not jsonl.exists():
        return None
    samples = []
    for line in jsonl.read_text().strip().split("\n"):
        if line.strip():
            samples.append(json.loads(line))
    meta = json.loads(meta_file.read_text()) if meta_file.exists() else {}
    return {"name": name, "samples": samples, "meta": meta}


def svg_chart(scenarios, key, title, ylabel, width=700, height=300, events=None):
    colors = {
        "trunk": "#e74c3c", "backpressure": "#f39c12",
        "discard": "#2ecc71", "discard-disconnect": "#3498db",
        "trunk-disconnect": "#c0392b", "backpressure-disconnect": "#e67e22",
    }
    ml, mr, mt, mb = 70, 140, 40, 50
    pw, ph = width - ml - mr, height - mt - mb

    all_t, all_v = [], []
    for sc in scenarios:
        for s in sc["samples"]:
            all_t.append(s["t"])
            v = s.get(key)
            if v is not None and str(v) != "null":
                try:
                    all_v.append(float(v))
                except (ValueError, TypeError):
                    pass
    if not all_v:
        return f"<p>No data for {title}</p>"

    t_min, t_max = min(all_t), max(all_t)
    v_max = max(all_v) * 1.1 or 1

    def tx(t): return ml + (t - t_min) / max(t_max - t_min, 1) * pw
    def ty(v): return mt + ph - v / v_max * ph

    out = [f'<svg width="{width}" height="{height}" xmlns="http://www.w3.org/2000/svg">',
           f'<rect width="{width}" height="{height}" fill="white"/>',
           f'<text x="{width//2}" y="20" text-anchor="middle" font-size="14" font-weight="bold" fill="#333">{title}</text>']

    for i in range(6):
        y = mt + i * ph / 5
        v = v_max * (5 - i) / 5
        out.append(f'<line x1="{ml}" y1="{y}" x2="{ml+pw}" y2="{y}" stroke="#eee"/>')
        out.append(f'<text x="{ml-5}" y="{y+4}" text-anchor="end" font-size="10" fill="#666">{v:.1f}</text>')

    step = max(int((t_max - t_min) / 8), 5)
    for t in range(0, int(t_max) + 1, step):
        x = tx(t)
        out.append(f'<line x1="{x}" y1="{mt+ph}" x2="{x}" y2="{mt+ph+5}" stroke="#666"/>')
        out.append(f'<text x="{x}" y="{mt+ph+18}" text-anchor="middle" font-size="10" fill="#666">{t}s</text>')

    out.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#333"/>')
    out.append(f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#333"/>')
    out.append(f'<text x="{width//2}" y="{height-5}" text-anchor="middle" font-size="11" fill="#333">Time (seconds)</text>')
    out.append(f'<text x="15" y="{height//2}" text-anchor="middle" font-size="11" fill="#333" transform="rotate(-90 15 {height//2})">{ylabel}</text>')

    if events:
        for evt_t, lbl, clr in events:
            x = tx(evt_t)
            out.append(f'<line x1="{x}" y1="{mt}" x2="{x}" y2="{mt+ph}" stroke="{clr}" stroke-width="2" stroke-dasharray="5,5"/>')
            out.append(f'<text x="{x+3}" y="{mt+12}" font-size="9" fill="{clr}">{lbl}</text>')

    ly = mt + 10
    for sc in scenarios:
        color = colors.get(sc["name"], "#999")
        pts = [(s["t"], float(s[key])) for s in sc["samples"]
               if s.get(key) is not None and str(s[key]) != "null"
               and _safe_float(s[key]) is not None]
        if not pts:
            continue
        path = " ".join(f"{'M' if i==0 else 'L'} {tx(t):.1f} {ty(v):.1f}" for i,(t,v) in enumerate(pts))
        out.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="2"/>')
        for t, v in pts:
            out.append(f'<circle cx="{tx(t):.1f}" cy="{ty(v):.1f}" r="3" fill="{color}"/>')
        lx = ml + pw + 10
        out.append(f'<line x1="{lx}" y1="{ly}" x2="{lx+20}" y2="{ly}" stroke="{color}" stroke-width="2"/>')
        out.append(f'<text x="{lx+25}" y="{ly+4}" font-size="10" fill="#333">{sc["name"]}</text>')
        ly += 18

    out.append("</svg>")
    return "\n".join(out)


def _safe_float(v):
    try:
        return float(v)
    except (ValueError, TypeError):
        return None


def generate_html(results_dir, output_path):
    scenarios = [s for s in [load_scenario(results_dir, n)
                 for n in ["trunk", "backpressure", "discard",
                           "trunk-disconnect", "backpressure-disconnect",
                           "discard-disconnect"]] if s]
    if not scenarios:
        print(f"No data in {results_dir}")
        sys.exit(1)

    events = [(30, "disconnect", "#e74c3c"), (90, "reconnect", "#2ecc71")]
    has_dc = any(s["name"] == "discard-disconnect" for s in scenarios)

    h = ['<!DOCTYPE html><html><head><meta charset="utf-8">',
         '<title>ET Flow Control E2E Report</title>',
         '''<style>
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;max-width:900px;margin:40px auto;padding:0 20px;color:#333;line-height:1.6}
h1{border-bottom:2px solid #2ecc71;padding-bottom:10px}
h2{color:#2c3e50;margin-top:40px}
table{border-collapse:collapse;width:100%;margin:20px 0}
th,td{border:1px solid #ddd;padding:8px 12px;text-align:left}
th{background:#f5f5f5}
.chart{margin:30px 0;text-align:center}
.good{color:#2ecc71;font-weight:bold}
.bad{color:#e74c3c;font-weight:bold}
code{background:#f5f5f5;padding:2px 6px;border-radius:3px;font-size:13px}
.meta{background:#f9f9f9;border-left:4px solid #3498db;padding:15px;margin:20px 0}
</style>''',
         '</head><body>',
         '<h1>ET Flow Control E2E Test Report</h1>',
         f'<p>Generated: {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}</p>',
         '<h2>Test Scenarios</h2>',
         '<table><tr><th>Scenario</th><th>Git Commit</th><th>Client Command</th><th>Proxy Rate</th><th>Server</th></tr>']

    for sc in scenarios:
        m = sc["meta"]
        alive = '<span class="good">alive</span>' if m.get("etserver_alive", True) else '<span class="bad">CRASHED</span>'
        rate = m.get("proxy_rate", "?")
        rate_str = f"{rate:,} B/s" if isinstance(rate, int) else str(rate)
        h.append(f'<tr><td><b>{sc["name"]}</b></td><td><code>{m.get("commit","?")}</code></td>'
                 f'<td><code>{m.get("et_command","?")}</code></td>'
                 f'<td>{rate_str}</td><td>{alive}</td></tr>')
    h.append('</table>')

    h.append('<div class="meta"><b>Setup:</b> etserver :4444 &rarr; throttle proxy :4445 &rarr; et client. '
             'Workload: <code>print_timestamps.py</code> ~2.7MB/s. Proxy limits to 100KB/s.</div>')

    h.append('<h2>Display Lag</h2>')
    h.append('<p>How stale the terminal output is. Grows ~1s/s in all modes (TCP buffer bottleneck).</p>')
    h.append(f'<div class="chart">{svg_chart(scenarios, "display_lag", "Display Lag Over Time", "Seconds behind real time", events=events if has_dc else None)}</div>')

    h.append('<h2>Process Lag</h2>')
    h.append('<p>Whether the producing process is stalled. <span class="good">0 = running freely</span>, <span class="bad">&gt;1 = blocked on write()</span>.</p>')
    h.append(f'<div class="chart">{svg_chart(scenarios, "process_lag", "Process Lag Over Time", "Seconds behind real time", events=events if has_dc else None)}</div>')

    h.append('<h2>Process Throughput</h2>')
    h.append('<p>Lines/sec written by the producing process. Higher = process running freely.</p>')
    h.append(f'<div class="chart">{svg_chart(scenarios, "lines_sec", "Process Throughput", "Lines/sec", events=events if has_dc else None)}</div>')

    h.append('<h2>Interpretation</h2>')
    h.append('<h3>Display lag is a network property</h3>')
    h.append('<p>All modes show ~1s/s growth because the proxy limits throughput. TCP is ordered &mdash; '
             'data in the kernel buffer must be delivered in sequence. No application-level change can fix this.</p>')
    h.append('<h3>Process lag is the key differentiator</h3>')
    h.append('<p><b>Trunk &amp; backpressure:</b> process stalls intermittently (process_lag &gt; 0) when TCP buffer fills.</p>')
    h.append('<p><b>Discard:</b> process_lag stays at <span class="good">0</span>. Old data dropped, PTY always drained.</p>')

    if has_dc:
        h.append('<h3>Disconnect scenario</h3>')
        h.append('<p>At t=30s, proxy kills TCP connections for 60s. At t=90s, proxy resumes.</p>')
        h.append('<p>In <b>discard</b> mode, the process keeps running during disconnect. '
                 'WriteBuffer discards old output. On reconnect, client sees recent data.</p>')

    h.append('<h3>When to use each mode</h3>')
    h.append('<table><tr><th>Mode</th><th>Best for</th><th>Trade-off</th></tr>')
    h.append('<tr><td>discard (default)</td><td>Long-running jobs, builds</td><td>Old output lost</td></tr>')
    h.append('<tr><td>backpressure</td><td>tmux -CC, stateful protocols</td><td>Process stalls</td></tr></table>')
    h.append('</body></html>')

    Path(output_path).write_text("\n".join(h))
    print(f"Report: {output_path}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 generate_report.py <results_dir> <output.html>")
        sys.exit(1)
    generate_html(sys.argv[1], sys.argv[2])
