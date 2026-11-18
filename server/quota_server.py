import argparse
import datetime
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import threading
import time

DEFAULT_PORT = int(os.environ.get("PORT", "8787"))
DEFAULT_INTERVAL = int(os.environ.get("POLL_INTERVAL", "60"))

_lock = threading.Lock()
_cache = {
    "plan": "AGY",
    "status": "INIT",
    "updatedAt": None,
    "items": [],
    "line": "QUOTA:AGY|INIT|Waiting:0:Reset: -",
    "error": None,
}


def find_agy_binary() -> str:
    env_bin = os.environ.get("AGY_BIN")
    if env_bin and os.path.isfile(env_bin):
        return env_bin
    found = shutil.which("agy")
    if found:
        return found
    local_app = os.environ.get("LOCALAPPDATA", "")
    candidate = os.path.join(local_app, "agy", "bin", "agy.exe")
    if os.path.isfile(candidate):
        return candidate
    return "agy"


def format_reset_delta(iso_str: str) -> str:
    if not iso_str:
        return "Reset: -"
    try:
        s = iso_str.strip()
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        target = datetime.datetime.fromisoformat(s)
        if target.tzinfo is None:
            target = target.replace(tzinfo=datetime.timezone.utc)
        now = datetime.datetime.now(datetime.timezone.utc)
        secs = int((target - now).total_seconds())
        if secs <= 0:
            return "Reset: soon"
        days, rem = divmod(secs, 86400)
        hours, rem = divmod(rem, 3600)
        mins, _ = divmod(rem, 60)
        if days > 0:
            return f"Reset: {days}d {hours}h"
        if hours > 0:
            return f"Reset: {hours}h {mins}m"
        return f"Reset: {mins}m"
    except Exception:
        return "Reset: -"


def fetch_agy_quota() -> dict:
    agy_bin = find_agy_binary()
    kwargs = {
        "capture_output": True,
        "text": True,
        "timeout": 20,
        "encoding": "utf-8",
        "errors": "replace",
    }
    if sys.platform == "win32":
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW

    proc = subprocess.run([agy_bin, "-p", "/usage", "--output-format", "json"], **kwargs)
    if proc.returncode != 0 or not proc.stdout.strip():
        raise RuntimeError(proc.stderr.strip() or f"agy exited with code {proc.returncode}")

    payload = json.loads(proc.stdout)
    groups = payload.get("command", {}).get("data", {}).get("groups", [])
    if not groups:
        raise RuntimeError("No quota groups found in agy output")

    items = []
    for g in groups:
        g_name = g.get("name", "")
        buckets = g.get("buckets", [])
        if "Gemini Models" in g_name:
            for b in buckets:
                pct = round(float(b.get("remaining_fraction", 0)) * 100)
                rst = format_reset_delta(b.get("reset_time", ""))
                win = b.get("window", "")
                if win == "5h":
                    items.append({"name": "Gemini 5h", "pct": pct, "reset": rst})
                elif win == "weekly":
                    items.append({"name": "Gemini Weekly", "pct": pct, "reset": rst})
        elif "Claude" in g_name:
            chosen = None
            for b in buckets:
                if chosen is None or float(b.get("remaining_fraction", 1)) < float(chosen.get("remaining_fraction", 1)):
                    chosen = b
            if chosen:
                pct = round(float(chosen.get("remaining_fraction", 0)) * 100)
                rst = format_reset_delta(chosen.get("reset_time", ""))
                win_label = "5h" if chosen.get("window") == "5h" else "Weekly"
                items.append({"name": f"Claude/GPT {win_label}", "pct": pct, "reset": rst})
        elif "Argon" in g_name:
            if buckets:
                b = buckets[0]
                pct = round(float(b.get("remaining_fraction", 0)) * 100)
                rst = format_reset_delta(b.get("reset_time", ""))
                items.append({"name": "Gemini 4 Argon", "pct": pct, "reset": rst})

    items = items[:4]
    if not items:
        raise RuntimeError("Parsed 0 quota items")

    line_items = ";".join(f"{it['name']}:{it['pct']}:{it['reset']}" for it in items)
    now_iso = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    return {
        "plan": "AGY",
        "status": "LIVE",
        "updatedAt": now_iso,
        "items": items,
        "line": f"QUOTA:AGY|LIVE|{line_items}",
        "error": None,
    }


def refresh_cache() -> None:
    try:
        result = fetch_agy_quota()
        with _lock:
            _cache.update(result)
    except Exception as exc:
        with _lock:
            _cache["status"] = "ERR"
            _cache["error"] = str(exc)


def poller_loop(interval_sec: int) -> None:
    while True:
        refresh_cache()
        time.sleep(interval_sec)


class QuotaHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def _send(self, code: int, content_type: str, body: str):
        raw = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/refresh":
            refresh_cache()
            with _lock:
                data = dict(_cache)
            self._send(200, "application/json; charset=utf-8", json.dumps(data, ensure_ascii=False))
        elif path == "/quota":
            with _lock:
                data = dict(_cache)
            self._send(200, "application/json; charset=utf-8", json.dumps(data, ensure_ascii=False))
        elif path == "/quota.txt":
            with _lock:
                line = _cache["line"]
            self._send(200, "text/plain; charset=utf-8", line + "\n")
        elif path == "/health":
            with _lock:
                ok = _cache["error"] is None
                updated = _cache["updatedAt"]
                err = _cache["error"]
            self._send(200, "application/json; charset=utf-8", json.dumps({"ok": ok, "updatedAt": updated, "error": err}))
        else:
            self._send(404, "text/plain; charset=utf-8", "Not found\n")


class ReusableTCPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description="Antigravity Quota HTTP Server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--interval", type=int, default=DEFAULT_INTERVAL)
    args = parser.parse_args()

    t = threading.Thread(target=poller_loop, args=(args.interval,), daemon=True)
    t.start()

    srv = ReusableTCPServer((args.host, args.port), QuotaHandler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        srv.shutdown()


if __name__ == "__main__":
    main()
