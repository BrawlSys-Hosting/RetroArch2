#!/usr/bin/env python3
import json
import os
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn


def _load_env_file():
    env_path = os.path.join(os.path.dirname(__file__), ".env")
    if not os.path.exists(env_path):
        return
    try:
        with open(env_path, "r", encoding="utf-8") as handle:
            for line in handle:
                entry = line.strip()
                if not entry or entry.startswith("#") or "=" not in entry:
                    continue
                key, value = entry.split("=", 1)
                key = key.strip()
                value = value.strip()
                if not key or key in os.environ:
                    continue
                if len(value) >= 2 and value[0] == value[-1] and value[0] in ("\"", "'"):
                    value = value[1:-1]
                os.environ[key] = value
    except OSError:
        return


_load_env_file()

HOST_METHOD_UNKNOWN = 0
HOST_METHOD_MANUAL = 1
HOST_METHOD_UPNP = 2
HOST_METHOD_MITM = 3

ROOM_TTL_SECONDS = int(os.getenv("LOBBY_ROOM_TTL", "180"))
MAX_ROOMS = int(os.getenv("LOBBY_MAX_ROOMS", "512"))
MITM_CONFIG_PATH = os.getenv("LOBBY_MITM_CONFIG", "mitm_servers.json")

_rooms_by_id = {}
_rooms_by_key = {}
_next_id = 1
_rooms_lock = threading.Lock()


def _now():
    return int(time.time())


def _coerce_bool(value):
    if value is None:
        return False
    return value.strip().lower() in ("1", "true", "yes", "on")


def _coerce_int(value, default=0):
    if value is None or value == "":
        return default
    try:
        if isinstance(value, int):
            return value
        return int(value, 10)
    except (TypeError, ValueError):
        return default


def _coerce_hex(value):
    if not value:
        return ""
    return value.strip().upper()


def _load_mitm_config():
    if not os.path.exists(MITM_CONFIG_PATH):
        return {}
    try:
        with open(MITM_CONFIG_PATH, "r", encoding="utf-8") as handle:
            data = json.load(handle)
            if isinstance(data, dict):
                return data
    except (OSError, json.JSONDecodeError):
        return {}
    return {}


def _prune_rooms():
    cutoff = _now() - ROOM_TTL_SECONDS
    for room_id in list(_rooms_by_id.keys()):
        if _rooms_by_id[room_id]["updated"] < cutoff:
            key = _rooms_by_id[room_id]["key"]
            _rooms_by_id.pop(room_id, None)
            _rooms_by_key.pop(key, None)


def _room_key(fields):
    return "{ip}:{port}:{user}:{crc}".format(
        ip=fields.get("ip", ""),
        port=fields.get("port", 0),
        user=fields.get("username", ""),
        crc=fields.get("game_crc", ""),
    )


def _extract_fields(params, client_ip):
    fields = {}

    fields["username"] = params.get("username", "")
    fields["core_name"] = params.get("core_name", "")
    fields["core_version"] = params.get("core_version", "")
    fields["game_name"] = params.get("game_name", "")
    fields["game_crc"] = _coerce_hex(params.get("game_crc", ""))
    fields["port"] = _coerce_int(params.get("port"), 0)
    fields["retroarch_version"] = params.get("retroarch_version", "")
    fields["frontend"] = params.get("frontend", "")
    fields["subsystem_name"] = params.get("subsystem_name", "")
    fields["player_count"] = _coerce_int(params.get("player_count"), 0)
    fields["spectator_count"] = _coerce_int(params.get("spectator_count"), 0)
    fields["has_password"] = _coerce_bool(params.get("has_password"))
    fields["has_spectate_password"] = _coerce_bool(
        params.get("has_spectate_password")
    )
    fields["ggpo"] = _coerce_bool(params.get("ggpo"))
    fields["rendezvous"] = _coerce_bool(params.get("rendezvous"))
    fields["rendezvous_server"] = params.get("rendezvous_server", "")
    fields["rendezvous_room"] = params.get("rendezvous_room", "")
    fields["rendezvous_port"] = _coerce_int(params.get("rendezvous_port"), 0)
    fields["ggpo_relay"] = _coerce_bool(
        params.get("ggpo_relay", params.get("use_ggpo_relay"))
    )
    fields["ggpo_relay_server"] = params.get("ggpo_relay_server", "")
    fields["ggpo_relay_session"] = params.get("ggpo_relay_session", "")
    fields["ggpo_relay_port"] = _coerce_int(params.get("ggpo_relay_port"), 0)
    fields["mitm_server"] = params.get("mitm_server", "")
    fields["ip"] = client_ip
    fields["connectable"] = True
    fields["is_retroarch"] = True

    force_mitm = _coerce_bool(params.get("force_mitm"))
    fields["host_method"] = HOST_METHOD_MITM if force_mitm else HOST_METHOD_MANUAL

    mitm_addr = params.get("mitm_custom_addr", "")
    mitm_port = _coerce_int(params.get("mitm_custom_port"), 0)
    if mitm_addr:
        fields["mitm_ip"] = mitm_addr
        fields["mitm_port"] = mitm_port
    elif fields["mitm_server"]:
        mitm = _load_mitm_config()
        entry = mitm.get(fields["mitm_server"], {})
        fields["mitm_ip"] = entry.get("addr", "")
        fields["mitm_port"] = _coerce_int(entry.get("port"), 0)
    else:
        fields["mitm_ip"] = ""
        fields["mitm_port"] = 0
    fields["mitm_session"] = params.get("mitm_session", "")

    fields["country"] = params.get("country", "")

    return fields


def _plain_response(fields, room_id):
    lines = [
        "id={}".format(room_id),
        "username={}".format(fields.get("username", "")),
        "core_name={}".format(fields.get("core_name", "")),
        "core_version={}".format(fields.get("core_version", "")),
        "game_name={}".format(fields.get("game_name", "")),
        "game_crc={}".format(fields.get("game_crc", "")),
        "retroarch_version={}".format(fields.get("retroarch_version", "")),
        "frontend={}".format(fields.get("frontend", "")),
        "subsystem_name={}".format(fields.get("subsystem_name", "")),
        "ip={}".format(fields.get("ip", "")),
        "port={}".format(fields.get("port", 0)),
        "host_method={}".format(fields.get("host_method", HOST_METHOD_UNKNOWN)),
        "ggpo={}".format(1 if fields.get("ggpo") else 0),
        "rendezvous={}".format(1 if fields.get("rendezvous") else 0),
        "rendezvous_server={}".format(fields.get("rendezvous_server", "")),
        "rendezvous_room={}".format(fields.get("rendezvous_room", "")),
        "rendezvous_port={}".format(fields.get("rendezvous_port", 0)),
        "ggpo_relay={}".format(1 if fields.get("ggpo_relay") else 0),
        "ggpo_relay_server={}".format(fields.get("ggpo_relay_server", "")),
        "ggpo_relay_session={}".format(fields.get("ggpo_relay_session", "")),
        "ggpo_relay_port={}".format(fields.get("ggpo_relay_port", 0)),
        "has_password={}".format(1 if fields.get("has_password") else 0),
        "has_spectate_password={}".format(
            1 if fields.get("has_spectate_password") else 0
        ),
        "country={}".format(fields.get("country", "")),
        "connectable={}".format(1 if fields.get("connectable") else 0),
    ]
    return "\n".join(lines) + "\n"


class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


class LobbyHandler(BaseHTTPRequestHandler):
    server_version = "RetroArchLobby/1.0"

    def log_message(self, fmt, *args):
        return

    def _send(self, code, body, content_type="text/plain"):
        if isinstance(body, str):
            body = body.encode("utf-8", "replace")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/add":
            self._send(404, "Not Found\n")
            return

        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        params = urllib.parse.parse_qs(raw, keep_blank_values=True)
        flat = {k: v[0] if v else "" for k, v in params.items()}

        client_ip = self.client_address[0]
        fields = _extract_fields(flat, client_ip)

        with _rooms_lock:
            _prune_rooms()
            key = _room_key(fields)
            room_id = _rooms_by_key.get(key)
            if room_id is None:
                global _next_id
                if len(_rooms_by_id) >= MAX_ROOMS:
                    self._send(503, "Server Full\n")
                    return
                room_id = _next_id
                _next_id += 1
                _rooms_by_key[key] = room_id

            _rooms_by_id[room_id] = {
                "id": room_id,
                "key": key,
                "fields": fields,
                "updated": _now(),
            }

        self._send(200, _plain_response(fields, room_id))

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/list":
            with _rooms_lock:
                _prune_rooms()
                records = []
                for room in _rooms_by_id.values():
                    records.append({"fields": room["fields"]})
            payload = json.dumps({"records": records}, separators=(",", ":"))
            self._send(200, payload, "application/json")
            return

        if parsed.path == "/tunnel":
            params = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
            name = params.get("name", [""])[0]
            mitm = _load_mitm_config()
            entry = mitm.get(name, {})
            addr = entry.get("addr", "")
            port = entry.get("port", 0)
            body = "tunnel_addr={}\n".format(addr)
            body += "tunnel_port={}\n".format(port)
            self._send(200, body)
            return

        self._send(404, "Not Found\n")


def main():
    host = os.getenv("LOBBY_BIND", "0.0.0.0")
    port = int(os.getenv("LOBBY_PORT", "55435"))
    server = ThreadingHTTPServer((host, port), LobbyHandler)
    print("Lobby server listening on {}:{} (TTL={}s)".format(
        host, port, ROOM_TTL_SECONDS
    ))
    server.serve_forever()


if __name__ == "__main__":
    main()
