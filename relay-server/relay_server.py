#!/usr/bin/env python3
import os
import socket
import time

DEFAULT_MAGIC = "RARELAY1"

DEFAULT_BIND = "0.0.0.0"
DEFAULT_PORT = 7001
DEFAULT_SESSION_TTL = 120.0
DEFAULT_CLIENT_TTL = 30.0
DEFAULT_MAX_SESSIONS = 512
DEFAULT_MAX_PACKET = 8192


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


def _now():
    return time.monotonic()


def _load_magic():
    raw = os.getenv("RELAY_MAGIC")
    if not raw:
        return DEFAULT_MAGIC
    if " " in raw:
        return DEFAULT_MAGIC
    try:
        raw.encode("ascii")
    except UnicodeEncodeError:
        return DEFAULT_MAGIC
    return raw


MAGIC_TEXT = _load_magic()
MAGIC = MAGIC_TEXT.encode("ascii")
MAGIC_PREFIX = MAGIC + b" "


def _get_env_float(name, default):
    raw = os.getenv(name)
    if raw is None:
        return float(default)
    try:
        return float(raw)
    except ValueError:
        return float(default)


def _get_env_int(name, default):
    raw = os.getenv(name)
    if raw is None:
        return int(default)
    try:
        return int(raw, 10)
    except ValueError:
        return int(default)


def _parse_cmd(data):
    if not data.startswith(MAGIC_PREFIX):
        return None
    try:
        text = data.decode("ascii", "strict")
    except UnicodeDecodeError:
        return None
    parts = text.strip().split()
    if len(parts) < 3:
        return None
    if parts[0] != MAGIC_TEXT:
        return None
    cmd = parts[1].upper()
    session = parts[2]
    slot = None
    if len(parts) >= 4:
        slot = parts[3]
    return cmd, session, slot


def _send_response(sock, addr, text):
    payload = (text + "\n").encode("ascii", "replace")
    sock.sendto(payload, addr)


def _remove_client(address_map, sessions, addr):
    entry = address_map.pop(addr, None)
    if not entry:
        return
    session_id, slot = entry
    session = sessions.get(session_id)
    if not session:
        return
    if session["clients"].get(slot, {}).get("addr") == addr:
        session["clients"][slot] = None


def _prune_sessions(address_map, sessions, now, client_ttl, session_ttl):
    for session_id in list(sessions.keys()):
        session = sessions[session_id]
        for slot, client in list(session["clients"].items()):
            if not client:
                continue
            if now - client["last_seen"] > client_ttl:
                address_map.pop(client["addr"], None)
                session["clients"][slot] = None
        if session["clients"][1] is None and session["clients"][2] is None:
            if now - session["updated"] > session_ttl:
                sessions.pop(session_id, None)


def main():
    bind_addr = os.getenv("RELAY_BIND", DEFAULT_BIND)
    port = _get_env_int("RELAY_PORT", DEFAULT_PORT)
    session_ttl = _get_env_float("RELAY_SESSION_TTL", DEFAULT_SESSION_TTL)
    client_ttl = _get_env_float("RELAY_CLIENT_TTL", DEFAULT_CLIENT_TTL)
    max_sessions = _get_env_int("RELAY_MAX_SESSIONS", DEFAULT_MAX_SESSIONS)
    max_packet = _get_env_int("RELAY_MAX_PACKET", DEFAULT_MAX_PACKET)

    sessions = {}
    address_map = {}
    last_prune = 0.0

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_addr, port))
    sock.settimeout(1.0)

    print("GGPO relay listening on {}:{} (ttl={}s)".format(
        bind_addr, port, session_ttl
    ))

    while True:
        try:
            data, addr = sock.recvfrom(max_packet)
        except socket.timeout:
            data = None
            addr = None
        except OSError:
            continue

        now = _now()
        if now - last_prune > 1.0:
            _prune_sessions(address_map, sessions, now, client_ttl, session_ttl)
            last_prune = now

        if not data:
            continue

        cmd = _parse_cmd(data)
        if cmd:
            command, session_id, slot_token = cmd
            session = sessions.get(session_id)
            if command == "HELLO":
                if session is None:
                    if len(sessions) >= max_sessions:
                        _send_response(sock, addr, "{} FULL {}".format(
                            MAGIC_TEXT, session_id))
                        continue
                    session = {
                        "clients": {1: None, 2: None},
                        "updated": now,
                    }
                    sessions[session_id] = session

                if slot_token is not None:
                    if slot_token not in ("1", "2"):
                        _send_response(sock, addr, "{} ERR {} bad_slot".format(
                            MAGIC_TEXT, session_id))
                        continue
                    slot = int(slot_token)
                else:
                    slot = 1 if session["clients"][1] is None else 2

                current = session["clients"].get(slot)
                if current and current["addr"] != addr:
                    _send_response(sock, addr, "{} BUSY {}".format(
                        MAGIC_TEXT, session_id))
                    continue

                _remove_client(address_map, sessions, addr)
                session["clients"][slot] = {"addr": addr, "last_seen": now}
                session["updated"] = now
                address_map[addr] = (session_id, slot)

                ready = session["clients"][1] and session["clients"][2]
                status = "READY" if ready else "WAIT"
                _send_response(sock, addr, "{} {} {} {}".format(
                    MAGIC_TEXT, status, session_id, slot
                ))
                continue

            if command == "BYE":
                _remove_client(address_map, sessions, addr)
                _send_response(sock, addr, "{} OK {}".format(
                    MAGIC_TEXT, session_id))
                continue

            if command == "PING":
                entry = address_map.get(addr)
                if entry:
                    session_id, slot = entry
                    session = sessions.get(session_id)
                    if session and session["clients"].get(slot):
                        session["clients"][slot]["last_seen"] = now
                        session["updated"] = now
                _send_response(sock, addr, "{} PONG {}".format(
                    MAGIC_TEXT, session_id))
                continue

            _send_response(sock, addr, "{} ERR {} unknown_command".format(
                MAGIC_TEXT, session_id))
            continue

        entry = address_map.get(addr)
        if not entry:
            continue
        session_id, slot = entry
        session = sessions.get(session_id)
        if not session:
            continue
        client = session["clients"].get(slot)
        if not client:
            continue
        client["last_seen"] = now
        session["updated"] = now

        other_slot = 2 if slot == 1 else 1
        other = session["clients"].get(other_slot)
        if not other:
            continue
        try:
            sock.sendto(data, other["addr"])
        except OSError:
            continue


if __name__ == "__main__":
    main()
