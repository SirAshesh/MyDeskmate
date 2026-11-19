import asyncio
import io
import json
import os
import re
import struct
import time
from typing import Dict, List, Optional, Set, Tuple

import aiohttp
from aiohttp import web
from PIL import Image
from discord_bridge import register_discord_routes

PORT = int(os.environ.get("PORT", "8765"))
THUMB_SIZE = 128

# Connected clients by role
youtube_clients: Set[web.WebSocketResponse] = set()
esp32_clients: Set[web.WebSocketResponse] = set()
pending_yt_cmds: List[str] = []
last_yt_http_ms = 0.0

# Caches
lyrics_cache: Dict[str, List[Tuple[float, str]]] = {}
lyrics_fetching: Set[str] = set()
thumb_cache: Dict[str, bytes] = {}

# Current playback state
current_state = {
    "videoId": "",
    "title": "Tidak ada lagu",
    "artist": "-",
    "time": 0.0,
    "duration": 0.0,
    "paused": True,
}
last_broadcast_ms = 0.0
last_broadcast_idx = -999
last_broadcast_vid = ""
last_broadcast_paused = None

LRC_LINE_RE = re.compile(r"\[(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?\]\s*(.*)")
NOISE_RE = re.compile(
    r"(\(|\[)[^\)\]]*(official|music\s*video|lyric|lyrics|audio|mv|hd|4k|visualizer|live|feat\.?|ft\.?)[^\)\]]*(\)|\])",
    re.IGNORECASE,
)


def clean_youtube_title(raw_title: str, channel: str) -> Tuple[str, str]:
    cleaned = NOISE_RE.sub("", raw_title)
    cleaned = re.sub(r"\s+", " ", cleaned).strip(" -|•")
    if " - " in cleaned:
        parts = cleaned.split(" - ", 1)
        return parts[0].strip(), parts[1].strip()
    artist = re.sub(r"\s*-\s*Topic$", "", channel, flags=re.IGNORECASE).strip()
    return artist, cleaned


def parse_lrc(lrc_text: str) -> List[Tuple[float, str]]:
    lines: List[Tuple[float, str]] = []
    for raw in lrc_text.splitlines():
        m = LRC_LINE_RE.match(raw.strip())
        if not m:
            continue
        mins = int(m.group(1))
        secs = int(m.group(2))
        frac_str = m.group(3) or "0"
        frac = int(frac_str) / (10 ** len(frac_str))
        ts = mins * 60.0 + secs + frac
        text = m.group(4).strip()
        if text:
            lines.append((ts, text))
    lines.sort(key=lambda x: x[0])
    return lines


def score_lrc_candidate(item: dict, track: str, artist: str, duration: float) -> float:
    synced = item.get("syncedLyrics") or ""
    if not synced:
        return -1e9
    score = 0.0
    t_name = str(item.get("trackName") or "").lower()
    a_name = str(item.get("artistName") or "").lower()
    req_t = track.lower()
    req_a = artist.lower()

    if req_t and t_name == req_t:
        score += 40.0
    elif req_t and req_t in t_name:
        score += 15.0

    if req_a and a_name == req_a:
        score += 30.0
    elif req_a and (req_a in a_name or a_name in req_a):
        score += 12.0

    # Hindari versi terjemahan / romanized / cover jika judul asli tidak memintanya
    for bad in ("translated", "translation", "terjemahan", "indo", "romanized", "romaji", "english ver", "cover"):
        if bad in t_name and bad not in req_t:
            score -= 50.0

    item_dur = float(item.get("duration") or 0.0)
    if duration > 5.0 and item_dur > 5.0:
        diff = abs(item_dur - duration)
        if diff <= 3.0:
            score += 35.0
        elif diff <= 7.0:
            score += 15.0
        else:
            score -= min(40.0, diff)
    return score


async def fetch_lyrics_for_video(cache_key: str, raw_title: str, channel: str, duration: float = 0.0):
    if cache_key in lyrics_cache or cache_key in lyrics_fetching:
        return
    lyrics_fetching.add(cache_key)
    try:
        artist, track = clean_youtube_title(raw_title, channel)
        queries = []
        if artist and track:
            queries.append({"track_name": track, "artist_name": artist})
            queries.append({"q": f"{artist} {track}"})
        if track:
            queries.append({"q": track})

        best_lines: List[Tuple[float, str]] = []
        best_score = -1e8
        timeout = aiohttp.ClientTimeout(total=8)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            for params in queries:
                try:
                    async with session.get("https://lrclib.net/api/search", params=params) as resp:
                        if resp.status != 200:
                            continue
                        data = await resp.json()
                        if not isinstance(data, list):
                            continue
                        for item in data:
                            synced = item.get("syncedLyrics")
                            if not synced:
                                continue
                            parsed = parse_lrc(synced)
                            if not parsed:
                                continue
                            sc = score_lrc_candidate(item, track, artist, duration)
                            if sc > best_score:
                                best_score = sc
                                best_lines = parsed
                        if best_lines and best_score >= 40.0:
                            break
                except Exception as exc:
                    print(f"[LRCLIB] query error for {cache_key}: {exc}")

        if len(lyrics_cache) > 50:
            lyrics_cache.pop(next(iter(lyrics_cache)))
        lyrics_cache[cache_key] = best_lines
        print(f"[LRCLIB] {cache_key} ({artist} - {track}) -> {len(best_lines)} synced lines (score={best_score:.1f})")
        await broadcast_to_esp32(force=True)
    finally:
        lyrics_fetching.discard(cache_key)


def get_lyric_window(cache_key: str, cur_time: float) -> Tuple[bool, int, str, str, str, str]:
    lines = lyrics_cache.get(cache_key)
    if not lines:
        if cache_key in lyrics_fetching:
            return False, -1, "", "Mencari lirik...", "", ""
        return False, -1, "", "Lirik tidak tersedia", "", ""

    # Slight +0.25s lookahead for snappy Spotify-like sync
    t = cur_time + 0.25
    idx = -1
    for i, (ts, _) in enumerate(lines):
        if ts <= t:
            idx = i
        else:
            break

    prev_txt = lines[idx - 1][1] if idx - 1 >= 0 else ""
    curr_txt = lines[idx][1] if idx >= 0 else "..."
    next1_txt = lines[idx + 1][1] if idx + 1 < len(lines) else ""
    next2_txt = lines[idx + 2][1] if idx + 2 < len(lines) else ""
    return True, idx, prev_txt, curr_txt, next1_txt, next2_txt


async def broadcast_to_esp32(force: bool = False):
    global last_broadcast_ms, last_broadcast_idx, last_broadcast_vid, last_broadcast_paused

    if not esp32_clients:
        return

    vid = current_state["videoId"]
    title = current_state["title"]
    cache_key = f"{vid}::{title}"
    cur_time = float(current_state["time"])
    paused = bool(current_state["paused"])
    has_lyrics, idx, prev_txt, curr_txt, next1_txt, next2_txt = get_lyric_window(cache_key, cur_time)

    now_ms = time.monotonic() * 1000.0
    changed = (
        force
        or vid != last_broadcast_vid
        or idx != last_broadcast_idx
        or paused != last_broadcast_paused
    )
    if not changed and (now_ms - last_broadcast_ms) < 400.0:
        return

    last_broadcast_ms = now_ms
    last_broadcast_idx = idx
    last_broadcast_vid = vid
    last_broadcast_paused = paused

    payload = json.dumps(
        {
            "type": "state",
            "videoId": vid,
            "title": current_state["title"][:96],
            "artist": current_state["artist"][:64],
            "time": round(cur_time, 2),
            "duration": round(float(current_state["duration"]), 2),
            "paused": paused,
            "hasLyrics": has_lyrics,
            "lyricIdx": idx,
            "prev": prev_txt[:120],
            "curr": curr_txt[:120],
            "next1": next1_txt[:120],
            "next2": next2_txt[:120],
        },
        ensure_ascii=False,
    )

    dead = []
    for ws in list(esp32_clients):
        try:
            await ws.send_str(payload)
        except Exception:
            dead.append(ws)
    for ws in dead:
        esp32_clients.discard(ws)


async def send_cmd_to_youtube(cmd: str):
    pending_yt_cmds.append(cmd)
    if len(pending_yt_cmds) > 20:
        pending_yt_cmds.pop(0)
    if not youtube_clients:
        return
    msg = json.dumps({"cmd": cmd})
    dead = []
    for ws in list(youtube_clients):
        try:
            await ws.send_str(msg)
        except Exception:
            dead.append(ws)
    for ws in dead:
        youtube_clients.discard(ws)


async def process_youtube_state(data: dict):
    vid = str(data.get("videoId") or "")
    title = str(data.get("title") or "Tidak ada lagu").strip()
    artist = str(data.get("channel") or data.get("artist") or "-").strip()
    cur_t = float(data.get("time") or 0.0)
    dur = float(data.get("duration") or 0.0)
    paused = bool(data.get("paused", False))

    vid_changed = (vid != "" and vid != current_state["videoId"]) or (title != current_state["title"])

    current_state["videoId"] = vid or title[:32]
    current_state["title"] = title
    current_state["artist"] = artist
    current_state["time"] = cur_t
    current_state["duration"] = dur
    current_state["paused"] = paused

    if vid_changed:
        cache_key = f"{current_state['videoId']}::{title}"
        asyncio.create_task(fetch_lyrics_for_video(cache_key, title, artist, dur))

    await broadcast_to_esp32(force=vid_changed)


async def handle_yt_state_http(request: web.Request) -> web.Response:
    global last_yt_http_ms
    try:
        data = await request.json()
    except Exception:
        return web.json_response({"ok": False, "error": "invalid json"}, status=400)

    last_yt_http_ms = time.monotonic() * 1000.0
    if "title" in data and "time" in data:
        await process_youtube_state(data)

    cmds = list(pending_yt_cmds)
    pending_yt_cmds.clear()
    return web.json_response({"ok": True, "cmds": cmds})


async def handle_ws(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=20.0)
    await ws.prepare(request)

    role = "unknown"
    try:
        async for msg in ws:
            if msg.type != aiohttp.WSMsgType.TEXT:
                continue
            try:
                data = json.loads(msg.data)
            except Exception:
                continue

            if "role" in data:
                role = data["role"]
                if role == "youtube":
                    youtube_clients.add(ws)
                    esp32_clients.discard(ws)
                    print(f"[WS] YouTube connected (total={len(youtube_clients)})")
                elif role == "esp32":
                    esp32_clients.add(ws)
                    youtube_clients.discard(ws)
                    print(f"[WS] ESP32 connected (total={len(esp32_clients)})")
                    await broadcast_to_esp32(force=True)
                continue

            if "cmd" in data:
                cmd = str(data["cmd"])
                print(f"[CMD] ESP32 -> YouTube: {cmd}")
                await send_cmd_to_youtube(cmd)
                continue

            # Playback state from YouTube (either explicit role or legacy payload)
            if "title" in data and "time" in data:
                if role == "unknown":
                    role = "youtube"
                    youtube_clients.add(ws)
                await process_youtube_state(data)
    finally:
        youtube_clients.discard(ws)
        esp32_clients.discard(ws)
        if role != "unknown":
            print(f"[WS] {role} disconnected")

    return ws


async def handle_thumb(request: web.Request) -> web.Response:
    vid = request.query.get("v", "").strip()
    if not vid or not re.match(r"^[A-Za-z0-9_-]{6,20}$", vid):
        return web.Response(status=400, text="invalid videoId")

    if vid in thumb_cache:
        return web.Response(body=thumb_cache[vid], content_type="application/octet-stream")

    urls = [
        f"https://i.ytimg.com/vi/{vid}/hqdefault.jpg",
        f"https://i.ytimg.com/vi/{vid}/mqdefault.jpg",
    ]
    raw_bytes: Optional[bytes] = None
    timeout = aiohttp.ClientTimeout(total=8)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        for url in urls:
            try:
                async with session.get(url) as resp:
                    if resp.status == 200:
                        raw_bytes = await resp.read()
                        break
            except Exception:
                continue

    if not raw_bytes:
        return web.Response(status=404, text="thumbnail not found")

    try:
        img = Image.open(io.BytesIO(raw_bytes)).convert("RGB")
        w, h = img.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        img = img.crop((left, top, left + side, top + side))
        img = img.resize((THUMB_SIZE, THUMB_SIZE), Image.Resampling.LANCZOS)

        buf = bytearray(THUMB_SIZE * THUMB_SIZE * 2)
        idx = 0
        for r, g, b in img.getdata():
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into("<H", buf, idx, rgb565)
            idx += 2

        data = bytes(buf)
        if len(thumb_cache) > 25:
            thumb_cache.pop(next(iter(thumb_cache)))
        thumb_cache[vid] = data
        return web.Response(body=data, content_type="application/octet-stream")
    except Exception as exc:
        return web.Response(status=500, text=f"image error: {exc}")


async def handle_health(_request: web.Request) -> web.Response:
    yt_http_active = (time.monotonic() * 1000.0 - last_yt_http_ms) < 5000.0 if last_yt_http_ms > 0 else False
    return web.json_response(
        {
            "ok": True,
            "youtubeClients": len(youtube_clients) + (1 if yt_http_active else 0),
            "esp32Clients": len(esp32_clients),
            "videoId": current_state["videoId"],
            "title": current_state["title"],
        }
    )


async def root_handler(request: web.Request) -> web.StreamResponse:
    if request.headers.get("Upgrade", "").lower() == "websocket":
        return await handle_ws(request)
    return await handle_health(request)


def main():
    app = web.Application()
    app.router.add_get("/", root_handler)
    app.router.add_get("/ws", handle_ws)
    app.router.add_post("/yt/state", handle_yt_state_http)
    app.router.add_get("/thumb.rgb565", handle_thumb)
    app.router.add_get("/health", handle_health)
    register_discord_routes(app)
    print(f"[WS-SERVER] Listening on 0.0.0.0:{PORT}")
    web.run_app(app, host="0.0.0.0", port=PORT, print=None)


if __name__ == "__main__":
    main()
