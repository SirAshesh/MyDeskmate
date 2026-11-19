import asyncio
import base64
import hashlib
import json
import os
import time
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Optional

import aiohttp
from aiohttp import web
from cryptography.fernet import Fernet
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"
)
SUPER_PROPS = base64.b64encode(
    json.dumps(
        {
            "os": "Windows",
            "browser": "Chrome",
            "device": "",
            "system_locale": "en-US",
            "browser_user_agent": USER_AGENT,
            "browser_version": "128.0.0.0",
            "os_version": "10",
            "referrer": "",
            "referring_domain": "",
            "referrer_current": "",
            "referring_domain_current": "",
            "release_channel": "stable",
            "client_build_number": 325000,
            "client_event_source": None,
        },
        separators=(",", ":"),
    ).encode("utf-8")
).decode("ascii")
SESSION_FILE = os.environ.get("DISCORD_SESSION_FILE", "/app/data/discord_session.enc")
LEGACY_SESSION_FILE = "/app/data/discord_session.json"
KEY_FILE = os.environ.get("DISCORD_KEY_FILE", "/app/data/.session.key")
WITA_TZ = timezone(timedelta(hours=8))


class DiscordBridge:
    def __init__(self) -> None:
        self.token: str = ""
        self.username: str = ""
        self.state: str = "idle"
        self.qr_url: str = ""
        self.scanned_user: str = ""
        self.error: str = ""
        self._auth_task: Optional[asyncio.Task] = None
        self._channels_cache: List[Dict[str, str]] = []
        self._channels_cache_ts: float = 0.0
        self._load_session()

    def _get_fernet(self) -> Fernet:
        os.makedirs(os.path.dirname(KEY_FILE), exist_ok=True)
        if os.path.exists(KEY_FILE):
            with open(KEY_FILE, "rb") as f:
                key = f.read().strip()
        else:
            key = Fernet.generate_key()
            with open(KEY_FILE, "wb") as f:
                f.write(key)
            try:
                os.chmod(KEY_FILE, 0o600)
            except Exception:
                pass
        return Fernet(key)

    def _load_session(self) -> None:
        try:
            # Migrasi otomatis jika masih ada file plaintext lama
            if os.path.exists(LEGACY_SESSION_FILE) and not os.path.exists(SESSION_FILE):
                with open(LEGACY_SESSION_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                tok = (data.get("token") or "").strip()
                if tok:
                    self.token = tok
                    self.username = data.get("username", "")
                    self.state = "logged_in"
                    self._save_session()
                os.remove(LEGACY_SESSION_FILE)
                return

            if os.path.exists(SESSION_FILE):
                with open(SESSION_FILE, "rb") as f:
                    enc = f.read()
                raw = self._get_fernet().decrypt(enc).decode("utf-8")
                data = json.loads(raw)
                tok = (data.get("token") or "").strip()
                if tok:
                    self.token = tok
                    self.username = data.get("username", "")
                    self.state = "logged_in"
                    print(f"[DISCORD] Loaded encrypted session for {self.username or 'user'}")
        except Exception as e:
            print(f"[DISCORD] Failed to load session: {e}")

    def _save_session(self) -> None:
        try:
            os.makedirs(os.path.dirname(SESSION_FILE), exist_ok=True)
            payload = json.dumps({"token": self.token, "username": self.username}).encode("utf-8")
            enc = self._get_fernet().encrypt(payload)
            with open(SESSION_FILE, "wb") as f:
                f.write(enc)
            try:
                os.chmod(SESSION_FILE, 0o600)
            except Exception:
                pass
        except Exception as e:
            print(f"[DISCORD] Failed to save session: {e}")

    def logout(self) -> None:
        self.token = ""
        self.username = ""
        self.state = "idle"
        self.qr_url = ""
        self.scanned_user = ""
        self.error = ""
        self._channels_cache = []
        if self._auth_task and not self._auth_task.done():
            self._auth_task.cancel()
        try:
            if os.path.exists(SESSION_FILE):
                os.remove(SESSION_FILE)
        except Exception:
            pass

    async def login_with_token(self, token: str) -> Dict[str, Any]:
        tok = token.strip().strip('"').strip("'")
        if not tok:
            return {"ok": False, "error": "Token kosong"}

        timeout = aiohttp.ClientTimeout(total=10)
        headers = {"Authorization": tok, "User-Agent": USER_AGENT}
        async with aiohttp.ClientSession(timeout=timeout, headers=headers) as session:
            async with session.get("https://discord.com/api/v9/users/@me") as resp:
                if resp.status != 200:
                    return {"ok": False, "error": f"Token tidak valid (HTTP {resp.status})"}
                me = await resp.json()

        self.token = tok
        self.username = me.get("global_name") or me.get("username") or "Discord User"
        self.state = "logged_in"
        self.qr_url = ""
        self.error = ""
        self._channels_cache = []
        self._save_session()
        print(f"[DISCORD] Token login success for {self.username}")
        return {"ok": True, "username": self.username}

    def status_dict(self) -> Dict[str, Any]:
        return {
            "loggedIn": bool(self.token),
            "state": "logged_in" if self.token else self.state,
            "qrUrl": self.qr_url,
            "scannedUser": self.scanned_user,
            "username": self.username,
            "error": self.error,
        }

    async def start_qr_auth(self) -> Dict[str, Any]:
        if self.token:
            return self.status_dict()

        if self._auth_task and not self._auth_task.done():
            self._auth_task.cancel()
            try:
                await self._auth_task
            except BaseException:
                pass

        self.state = "connecting"
        self.qr_url = ""
        self.scanned_user = ""
        self.error = ""
        ready_ev = asyncio.Event()
        self._auth_task = asyncio.create_task(self._run_remote_auth(ready_ev))

        try:
            await asyncio.wait_for(ready_ev.wait(), timeout=6.0)
        except asyncio.TimeoutError:
            if self.state == "connecting":
                self.error = "Timeout connecting to Discord Remote Auth"
        return self.status_dict()

    async def _run_remote_auth(self, ready_ev: asyncio.Event) -> None:
        private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        pub_der = private_key.public_key().public_bytes(
            encoding=serialization.Encoding.DER,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        pub_b64 = base64.b64encode(pub_der).decode("ascii")

        def decrypt_oaep(b64_payload: str) -> bytes:
            raw = base64.b64decode(b64_payload)
            return private_key.decrypt(
                raw,
                padding.OAEP(
                    mgf=padding.MGF1(algorithm=hashes.SHA256()),
                    algorithm=hashes.SHA256(),
                    label=None,
                ),
            )

        headers = {"Origin": "https://discord.com", "User-Agent": USER_AGENT}
        timeout = aiohttp.ClientTimeout(total=150)
        hb_task: Optional[asyncio.Task] = None

        try:
            async with aiohttp.ClientSession(timeout=timeout, headers=headers) as session:
                async with session.ws_connect(
                    "wss://remote-auth-gateway.discord.gg/?v=2"
                ) as ws:
                    async def heartbeat_loop(interval_ms: int) -> None:
                        while True:
                            await asyncio.sleep(max(5.0, interval_ms / 1000.0))
                            await ws.send_json({"op": "heartbeat"})

                    async for msg in ws:
                        if msg.type != aiohttp.WSMsgType.TEXT:
                            continue
                        data = json.loads(msg.data)
                        op = data.get("op")

                        if op == "hello":
                            interval = int(data.get("heartbeat_interval", 41250))
                            hb_task = asyncio.create_task(heartbeat_loop(interval))
                            await ws.send_json({"op": "init", "encoded_public_key": pub_b64})

                        elif op == "nonce_proof":
                            nonce_bytes = decrypt_oaep(data["encrypted_nonce"])
                            digest = hashlib.sha256(nonce_bytes).digest()
                            proof = base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")
                            await ws.send_json({"op": "nonce_proof", "proof": proof})

                        elif op == "pending_remote_init":
                            fp = data.get("fingerprint", "")
                            self.qr_url = f"https://discord.com/ra/{fp}"
                            self.state = "qr_ready"
                            ready_ev.set()
                            print(f"[DISCORD] QR ready: {self.qr_url}")

                        elif op == "pending_ticket":
                            try:
                                payload = decrypt_oaep(data["encrypted_user_payload"]).decode("utf-8")
                                parts = payload.split(":")
                                self.scanned_user = parts[-1] if parts else "User"
                            except Exception:
                                self.scanned_user = "User"
                            self.state = "scanned"
                            print(f"[DISCORD] QR scanned by {self.scanned_user}")

                        elif op == "pending_login":
                            ticket = data.get("ticket", "")
                            login_headers = {
                                "Origin": "https://discord.com",
                                "Referer": "https://discord.com/login",
                                "User-Agent": USER_AGENT,
                                "X-Super-Properties": SUPER_PROPS,
                                "Content-Type": "application/json",
                            }
                            async with session.post(
                                "https://discord.com/api/v9/users/@me/remote-auth/login",
                                json={"ticket": ticket},
                                headers=login_headers,
                            ) as resp:
                                if resp.status != 200:
                                    err_body = await resp.text()
                                    print(f"[DISCORD] Login HTTP {resp.status}: {err_body[:240]}")
                                    self.state = "error"
                                    self.error = f"Login HTTP {resp.status}"
                                    break
                                login_data = await resp.json()
                            enc_token = login_data.get("encrypted_token", "")
                            token = decrypt_oaep(enc_token).decode("utf-8").strip()
                            self.token = token
                            self.username = self.scanned_user or "Discord User"
                            self.state = "logged_in"
                            self.qr_url = ""
                            self._save_session()
                            print(f"[DISCORD] Login success for {self.username}")
                            break

                        elif op == "cancel":
                            self.state = "expired"
                            self.qr_url = ""
                            break

        except asyncio.CancelledError:
            pass
        except Exception as e:
            self.state = "error"
            self.error = str(e)[:80]
            ready_ev.set()
            print(f"[DISCORD] Remote auth error: {e}")
        finally:
            if hb_task and not hb_task.done():
                hb_task.cancel()
            if not self.token and self.state == "qr_ready":
                self.state = "expired"
            ready_ev.set()

    def _api_headers(self) -> Dict[str, str]:
        return {
            "Authorization": self.token,
            "User-Agent": USER_AGENT,
        }

    async def get_guilds(self) -> List[Dict[str, str]]:
        if not self.token:
            return []
        guilds_out: List[Dict[str, str]] = [{"id": "dm", "name": "Direct Messages"}]
        timeout = aiohttp.ClientTimeout(total=10)
        async with aiohttp.ClientSession(timeout=timeout, headers=self._api_headers()) as session:
            async with session.get("https://discord.com/api/v9/users/@me/guilds") as resp:
                if resp.status == 401:
                    self.logout()
                    return []
                if resp.status == 200:
                    guilds = await resp.json()
                    for g in guilds[:100]:
                        guilds_out.append({
                            "id": str(g["id"]),
                            "name": (g.get("name") or "Server")[:34],
                        })
        return guilds_out

    async def get_channels(self, guild_id: str = "dm") -> List[Dict[str, str]]:
        if not self.token:
            return []

        channels: List[Dict[str, str]] = []
        timeout = aiohttp.ClientTimeout(total=10)
        async with aiohttp.ClientSession(timeout=timeout, headers=self._api_headers()) as session:
            if not guild_id or guild_id == "dm":
                async with session.get("https://discord.com/api/v9/users/@me/channels") as resp:
                    if resp.status == 401:
                        self.logout()
                        return []
                    if resp.status == 200:
                        dms = await resp.json()
                        dms.sort(key=lambda c: int(c.get("last_message_id") or 0), reverse=True)
                        for dm in dms[:24]:
                            recips = dm.get("recipients") or []
                            name = dm.get("name") or ", ".join(
                                r.get("global_name") or r.get("username") or "DM" for r in recips
                            )
                            channels.append({
                                "id": str(dm["id"]),
                                "name": f"@{name}"[:34],
                                "type": "dm",
                            })
            else:
                async with session.get(f"https://discord.com/api/v9/guilds/{guild_id}/channels") as cresp:
                    if cresp.status == 401:
                        self.logout()
                        return []
                    if cresp.status == 200:
                        gchs = await cresp.json()
                        text_chs = [c for c in gchs if c.get("type") in (0, 5)]
                        text_chs.sort(key=lambda c: c.get("position", 0))
                        for ch in text_chs[:64]:
                            channels.append({
                                "id": str(ch["id"]),
                                "name": f"#{ch.get('name', 'chat')}"[:34],
                                "type": "guild",
                            })

        self._channels_cache = channels
        self._channels_cache_ts = time.time()
        return channels

    async def get_messages(self, channel_id: str, limit: int = 12) -> Dict[str, Any]:
        if not self.token:
            return {"ok": False, "error": "not_logged_in", "messages": []}

        if not channel_id:
            chs = await self.get_channels()
            if not chs:
                return {"ok": False, "error": "no_channels", "messages": []}
            channel_id = chs[0]["id"]

        ch_name = channel_id
        for c in self._channels_cache:
            if c["id"] == channel_id:
                ch_name = c["name"]
                break

        timeout = aiohttp.ClientTimeout(total=10)
        async with aiohttp.ClientSession(timeout=timeout, headers=self._api_headers()) as session:
            url = f"https://discord.com/api/v9/channels/{channel_id}/messages?limit={limit}"
            async with session.get(url) as resp:
                if resp.status == 401:
                    self.logout()
                    return {"ok": False, "error": "token_expired", "messages": []}
                if resp.status != 200:
                    return {"ok": False, "error": f"HTTP {resp.status}", "messages": []}
                raw_msgs = await resp.json()

        msgs = []
        for m in reversed(raw_msgs):
            author_obj = m.get("author") or {}
            author = author_obj.get("global_name") or author_obj.get("username") or "User"
            ts_str = m.get("timestamp") or ""
            time_wita = "--:--"
            try:
                dt = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
                time_wita = dt.astimezone(WITA_TZ).strftime("%H:%M")
            except Exception:
                pass

            content = (m.get("content") or "").replace("\n", " ").strip()
            if not content:
                if m.get("attachments"):
                    content = "[Attachment]"
                elif m.get("embeds"):
                    content = "[Embed]"
                elif m.get("sticker_items"):
                    content = "[Sticker]"
                else:
                    content = "[System message]"

            msgs.append({
                "id": str(m.get("id", "")),
                "author": author[:36],
                "time": time_wita,
                "content": content[:220],
            })

        return {
            "ok": True,
            "channelId": channel_id,
            "channelName": ch_name,
            "messages": msgs,
        }


bridge = DiscordBridge()


def register_discord_routes(app: web.Application) -> None:
    async def handle_web_ui(request: web.Request) -> web.Response:
        status_text = (
            f"Terhubung sebagai <b>{bridge.username}</b>"
            if bridge.token
            else "Belum login"
        )
        html = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>MyDeskmate Discord Login</title>
<style>
body{{font-family:system-ui,sans-serif;background:#0e0e10;color:#ecebe8;max-width:480px;margin:40px auto;padding:20px}}
.card{{background:#151517;border:1px solid #2a2a2e;border-radius:12px;padding:20px}}
input{{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a2a2e;background:#0b0b0d;color:#ecebe8;margin:10px 0}}
button{{padding:10px 16px;border-radius:8px;border:1px solid #8cc5bf;background:#1f1f22;color:#8cc5bf;cursor:pointer;font-weight:600}}
.muted{{color:#8d8c89;font-size:13px;line-height:1.5}}
</style></head><body><div class="card">
<h2>MyDeskmate Discord</h2>
<p>Status: {status_text}</p>
<p class="muted">Jika login QR terkena Captcha dari Discord, tempel token Discord kamu sekali saja di sini agar ESP32 bisa langsung membaca pesan secara mandiri tanpa perlu membuka PC.</p>
<input id="tok" type="password" placeholder="Tempel Discord Token di sini...">
<button onclick="save()">Simpan & Hubungkan</button>
<p id="msg" class="muted"></p>
</div>
<script>
async function save(){{
  const tok=document.getElementById('tok').value;
  const r=await fetch('/discord/token',{{method:'POST',headers:{{'Content-Type':'application/json'}},body:JSON.stringify({{token:tok}})}});
  const d=await r.json();
  document.getElementById('msg').innerText=d.ok?('Berhasil login sebagai '+d.username+'! Cek layar ESP32.'):('Gagal: '+d.error);
}}
</script></body></html>"""
        return web.Response(text=html, content_type="text/html")

    async def handle_token_login(request: web.Request) -> web.Response:
        try:
            body = await request.json()
        except Exception:
            return web.json_response({"ok": False, "error": "Invalid JSON"}, status=400)
        res = await bridge.login_with_token(body.get("token", ""))
        return web.json_response(res, status=200 if res.get("ok") else 400)

    async def handle_status(request: web.Request) -> web.Response:
        return web.json_response(bridge.status_dict())

    async def handle_qr_start(request: web.Request) -> web.Response:
        res = await bridge.start_qr_auth()
        return web.json_response(res)

    async def handle_logout(request: web.Request) -> web.Response:
        bridge.logout()
        return web.json_response({"ok": True})

    async def handle_guilds(request: web.Request) -> web.Response:
        gs = await bridge.get_guilds()
        return web.json_response({"ok": bool(bridge.token), "guilds": gs})

    async def handle_channels(request: web.Request) -> web.Response:
        guild_id = request.query.get("guild_id", "dm").strip()
        chs = await bridge.get_channels(guild_id)
        return web.json_response({"ok": bool(bridge.token), "channels": chs})

    async def handle_messages(request: web.Request) -> web.Response:
        ch_id = request.query.get("channel_id", "").strip()
        try:
            limit = max(1, min(20, int(request.query.get("limit", "12"))))
        except Exception:
            limit = 12
        res = await bridge.get_messages(ch_id, limit=limit)
        return web.json_response(res)

    app.router.add_get("/discord", handle_web_ui)
    app.router.add_post("/discord/token", handle_token_login)
    app.router.add_get("/discord/status", handle_status)
    app.router.add_post("/discord/qr/start", handle_qr_start)
    app.router.add_post("/discord/logout", handle_logout)
    app.router.add_get("/discord/guilds", handle_guilds)
    app.router.add_get("/discord/channels", handle_channels)
    app.router.add_get("/discord/messages", handle_messages)
