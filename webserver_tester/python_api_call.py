#!/usr/bin/env python3
"""
call_logger - logs every HTTP request made to this server (regardless of
path or method) and shows them on a live web page: URL, time, source IP.

Log entries are persisted to a SQLite database (call_logger.db, created
automatically next to this script if it doesn't already exist).
"""

import os
import sqlite3
from datetime import datetime

from flask import Flask, jsonify, request, Response

app = Flask(__name__)

# How many of the most recent log entries to show/keep serving via the API.
MAX_ENTRIES = 1000

# Paths that power the UI itself and should NOT be recorded, so the log
# doesn't fill up with the page polling for its own updates.
EXCLUDED_PATHS = {"/api/logs", "/api/clear", "/favicon.ico"}

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "call_logger.db")


def get_db():
    # A fresh connection per call keeps this safe across Flask's
    # multi-threaded request handling. timeout=10 lets writers wait
    # briefly instead of immediately erroring on "database is locked".
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    first_time = not os.path.exists(DB_PATH)
    conn = get_db()
    try:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS logs (
                id     INTEGER PRIMARY KEY AUTOINCREMENT,
                time   TEXT NOT NULL,
                method TEXT NOT NULL,
                path   TEXT NOT NULL,
                ip     TEXT NOT NULL
            )
            """
        )
        conn.commit()
    finally:
        conn.close()
    if first_time:
        print(f"Created new database at {DB_PATH}")


def client_ip() -> str:
    """Best-effort real client IP, honoring a reverse proxy if present."""
    fwd = request.headers.get("X-Forwarded-For")
    if fwd:
        return fwd.split(",")[0].strip()
    return request.remote_addr or "unknown"


def record_request():
    entry_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    method = request.method
    path = request.full_path if request.query_string else request.path
    ip = client_ip()

    conn = get_db()
    try:
        conn.execute(
            "INSERT INTO logs (time, method, path, ip) VALUES (?, ?, ?, ?)",
            (entry_time, method, path, ip),
        )
        conn.commit()
    finally:
        conn.close()


@app.before_request
def log_all_requests():
    if request.path not in EXCLUDED_PATHS:
        record_request()


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Request Log</title>
<style>
  :root {
    --bg: #0f1115;
    --panel: #171a21;
    --border: #262a33;
    --text: #e6e8eb;
    --muted: #8b93a1;
    --accent: #4fd1c5;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }
  header {
    padding: 20px 28px;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 10px;
  }
  header h1 { font-size: 18px; margin: 0; font-weight: 600; }
  header .sub { color: var(--muted); font-size: 13px; }
  main { padding: 20px 28px; }
  .toolbar {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 14px;
    color: var(--muted);
    font-size: 13px;
  }
  button {
    background: var(--panel);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 6px 12px;
    border-radius: 6px;
    cursor: pointer;
    font-size: 13px;
  }
  button:hover { border-color: var(--accent); color: var(--accent); }
  table {
    width: 100%;
    border-collapse: collapse;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
  }
  th, td {
    text-align: left;
    padding: 10px 14px;
    border-bottom: 1px solid var(--border);
    font-size: 13px;
  }
  th {
    color: var(--muted);
    font-weight: 600;
    text-transform: uppercase;
    font-size: 11px;
    letter-spacing: 0.04em;
  }
  tr:last-child td { border-bottom: none; }
  td.method { font-family: ui-monospace, Menlo, Consolas, monospace; color: var(--accent); }
  td.path { font-family: ui-monospace, Menlo, Consolas, monospace; word-break: break-all; }
  td.ip { color: var(--muted); }
  #empty { color: var(--muted); padding: 30px; text-align: center; }
</style>
</head>
<body>
<header>
  <h1>Request Log</h1>
  <span class="sub">Every call to this server, live</span>
</header>
<main>
  <div class="toolbar">
    <span id="count">0 requests</span>
    <button onclick="clearLog()">Clear log</button>
  </div>
  <table id="log-table" style="display:none">
    <thead>
      <tr><th>Time</th><th>Method</th><th>Path</th><th>Source IP</th></tr>
    </thead>
    <tbody id="log-body"></tbody>
  </table>
  <div id="empty">No requests logged yet.</div>
</main>
<script>
async function refresh() {
  const res = await fetch('/api/logs');
  const data = await res.json();
  const body = document.getElementById('log-body');
  const table = document.getElementById('log-table');
  const empty = document.getElementById('empty');
  const count = document.getElementById('count');
  count.textContent = data.length + ' request' + (data.length === 1 ? '' : 's');
  if (data.length === 0) {
    table.style.display = 'none';
    empty.style.display = 'block';
    return;
  }
  table.style.display = 'table';
  empty.style.display = 'none';
  body.innerHTML = data.map(e => `
    <tr>
      <td>${e.time}</td>
      <td class="method">${e.method}</td>
      <td class="path">${e.path}</td>
      <td class="ip">${e.ip}</td>
    </tr>`).join('');
}
async function clearLog() {
  await fetch('/api/clear', { method: 'POST' });
  refresh();
}
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
"""


@app.route("/api/logs")
def api_logs():
    conn = get_db()
    try:
        rows = conn.execute(
            "SELECT time, method, path, ip FROM logs ORDER BY id DESC LIMIT ?",
            (MAX_ENTRIES,),
        ).fetchall()
    finally:
        conn.close()
    return jsonify([dict(row) for row in rows])


@app.route("/api/clear", methods=["POST"])
def api_clear():
    conn = get_db()
    try:
        conn.execute("DELETE FROM logs")
        conn.commit()
    finally:
        conn.close()
    return jsonify({"status": "cleared"})


@app.route("/", defaults={"_path": ""}, methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"])
@app.route("/<path:_path>", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"])
def catch_all(_path):
    if request.path == "/":
        return Response(PAGE, mimetype="text/html")
    # Any other URL is simply acknowledged - the point is that it got logged.
    return Response("OK\n", mimetype="text/plain")


init_db()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, threaded=True)
