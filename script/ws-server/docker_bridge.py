import asyncio
import json
import re
from typing import Any, Dict, List

import aiohttp
from aiohttp import web

DOCKER_SOCK = "/var/run/docker.sock"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def _docker_session(timeout_sec: float = 5.0) -> aiohttp.ClientSession:
    connector = aiohttp.UnixConnector(path=DOCKER_SOCK)
    timeout = aiohttp.ClientTimeout(total=timeout_sec)
    return aiohttp.ClientSession(connector=connector, timeout=timeout)


def _json_resp(data: Dict[str, Any]) -> web.Response:
    return web.json_response(data, dumps=lambda obj: json.dumps(obj, ensure_ascii=False))


def _strip_docker_mux_and_ansi(raw: bytes) -> str:
    """Bersihkan 8-byte header multiplexed Docker stream & ANSI color codes."""
    lines: List[str] = []
    i = 0
    n = len(raw)
    looks_muxed = n >= 8 and raw[0] in (1, 2) and raw[1:4] == b"\x00\x00\x00"
    if looks_muxed:
        while i + 8 <= n:
            stream_type = raw[i]
            size = int.from_bytes(raw[i + 4 : i + 8], "big")
            i += 8
            if stream_type not in (0, 1, 2) or size > n - i:
                chunk = raw[i - 8 :]
                text = chunk.decode("utf-8", errors="replace")
                lines.extend(text.splitlines())
                break
            chunk = raw[i : i + size].decode("utf-8", errors="replace")
            i += size
            lines.extend(chunk.splitlines())
    else:
        text = raw.decode("utf-8", errors="replace")
        lines = text.splitlines()

    cleaned: List[str] = []
    for ln in lines:
        s = ANSI_RE.sub("", ln).replace("\r", "").strip()
        if s:
            cleaned.append(s[:140])
    return "\n".join(cleaned[-18:])


async def list_containers() -> Dict[str, Any]:
    async with _docker_session(5.0) as session:
        async with session.get("http://localhost/containers/json?all=1") as resp:
            if resp.status != 200:
                return {"ok": False, "error": f"Docker HTTP {resp.status}", "containers": []}
            raw_list = await resp.json()

    items: List[Dict[str, str]] = []
    running_cnt = 0
    for c in raw_list:
        names = c.get("Names") or []
        name = (names[0].lstrip("/") if names else c.get("Id", "")[:12])[:30]
        state = (c.get("State") or "unknown").lower()
        status = (c.get("Status") or state)[:36]
        if state == "running":
            running_cnt += 1
        items.append({
            "id": str(c.get("Id", ""))[:12],
            "name": name,
            "state": state,
            "status": status,
        })

    # Taruh container running di atas, offline/exited di bawah, lalu urut nama
    items.sort(key=lambda x: (0 if x["state"] == "running" else 1, x["name"].lower()))
    return {
        "ok": True,
        "running": running_cnt,
        "total": len(items),
        "containers": items[:32],
    }


async def _fetch_logs_bytes(session: aiohttp.ClientSession, cid: str, tail: int) -> bytes:
    try:
        async with session.get(
            f"http://localhost/containers/{cid}/logs?stdout=1&stderr=1&tail={tail}&timestamps=0"
        ) as lresp:
            return await lresp.read() if lresp.status == 200 else b""
    except Exception:
        return b""


async def _fetch_stats_summary(session: aiohttp.ClientSession, cid: str) -> Dict[str, Any]:
    try:
        async with session.get(
            f"http://localhost/containers/{cid}/stats?stream=false",
            timeout=aiohttp.ClientTimeout(total=2.5),
        ) as resp:
            if resp.status != 200:
                return {"cpuPct": 0.0, "memMb": 0}
            st = await resp.json()
            mem_stats = st.get("memory_stats") or {}
            mem_usage = mem_stats.get("usage", 0)
            inner_stats = mem_stats.get("stats") or {}
            mem_cache = inner_stats.get("inactive_file", inner_stats.get("cache", 0))
            mem_mb = max(0, int((mem_usage - mem_cache) / (1024 * 1024)))

            cpu_stats = st.get("cpu_stats") or {}
            pre_stats = st.get("precpu_stats") or {}
            cpu_delta = (cpu_stats.get("cpu_usage") or {}).get("total_usage", 0) - (
                pre_stats.get("cpu_usage") or {}
            ).get("total_usage", 0)
            sys_delta = cpu_stats.get("system_cpu_usage", 0) - pre_stats.get("system_cpu_usage", 0)
            ncpu = cpu_stats.get("online_cpus") or len(
                (cpu_stats.get("cpu_usage") or {}).get("percpu_usage") or [1]
            )
            cpu_pct = 0.0
            if sys_delta > 0 and cpu_delta > 0:
                cpu_pct = round((cpu_delta / sys_delta) * ncpu * 100.0, 1)
            return {"cpuPct": cpu_pct, "memMb": mem_mb}
    except Exception:
        return {"cpuPct": 0.0, "memMb": 0}


async def get_container_logs(cid: str, tail: int = 18) -> Dict[str, Any]:
    if not cid:
        return {"ok": False, "error": "missing id"}

    async with _docker_session(8.0) as session:
        async with session.get(f"http://localhost/containers/{cid}/json") as iresp:
            if iresp.status != 200:
                return {"ok": False, "error": f"Inspect HTTP {iresp.status}"}
            info = await iresp.json()

        name = (info.get("Name") or cid).lstrip("/")[:30]
        state_obj = info.get("State") or {}
        state = (state_obj.get("Status") or "unknown").lower()
        health = ((state_obj.get("Health") or {}).get("Status") or "").lower()
        status_label = f"{state} ({health})" if health else state

        logs_coro = _fetch_logs_bytes(session, cid, tail)
        if state == "running":
            stats_coro = _fetch_stats_summary(session, cid)
        else:
            stats_coro = asyncio.sleep(0, result={"cpuPct": 0.0, "memMb": 0})

        raw_logs, stats = await asyncio.gather(logs_coro, stats_coro)

    logs_text = _strip_docker_mux_and_ansi(raw_logs)
    if not logs_text:
        logs_text = "(Tidak ada output log terbaru)"

    return {
        "ok": True,
        "id": cid[:12],
        "name": name,
        "state": state,
        "status": status_label,
        "cpuPct": stats["cpuPct"],
        "memMb": stats["memMb"],
        "logs": logs_text[:2000],
    }


async def restart_container(cid: str) -> Dict[str, Any]:
    if not cid:
        return {"ok": False, "error": "missing id"}
    async with _docker_session(15.0) as session:
        async with session.post(f"http://localhost/containers/{cid}/restart?t=5") as resp:
            if resp.status in (204, 200):
                return {"ok": True}
            return {"ok": False, "error": f"Restart HTTP {resp.status}"}


def register_docker_routes(app: web.Application) -> None:
    async def handle_containers(request: web.Request) -> web.Response:
        print(f"[DOCKER] GET /docker/containers from {request.remote}", flush=True)
        try:
            res = await list_containers()
            print(f"[DOCKER] -> ok={res.get('ok')} running={res.get('running')}/{res.get('total')}", flush=True)
            return _json_resp(res)
        except Exception as e:
            print(f"[DOCKER] containers err: {e}", flush=True)
            return _json_resp({"ok": False, "error": str(e)[:80], "containers": []})

    async def handle_logs(request: web.Request) -> web.Response:
        cid = request.query.get("id", "").strip()
        print(f"[DOCKER] GET /docker/logs id={cid} from {request.remote}", flush=True)
        try:
            tail = max(5, min(30, int(request.query.get("tail", "18"))))
        except Exception:
            tail = 18
        try:
            return _json_resp(await get_container_logs(cid, tail=tail))
        except Exception as e:
            print(f"[DOCKER] logs err: {e}", flush=True)
            return _json_resp({"ok": False, "error": str(e)[:80]})

    async def handle_restart(request: web.Request) -> web.Response:
        cid = request.query.get("id", "").strip()
        try:
            return _json_resp(await restart_container(cid))
        except Exception as e:
            return _json_resp({"ok": False, "error": str(e)[:80]})

    app.router.add_get("/docker/containers", handle_containers)
    app.router.add_get("/docker/logs", handle_logs)
    app.router.add_post("/docker/restart", handle_restart)
