#!/usr/bin/env python3
import asyncio
import base64
import ipaddress
import os
import struct
import time

MAGIC_SESSION = 0x52415453  # RATS
MAGIC_LINK = 0x5241544C     # RATL
MAGIC_ADDR = 0x52415441     # RATA
MAGIC_PING = 0x52415450     # RATP

ID_SIZE = 16
ADDR_SIZE = 16

DEFAULT_BIND = "0.0.0.0"
DEFAULT_PORT = 7002
DEFAULT_PENDING_TTL = 30.0
DEFAULT_MAX_SESSIONS = 512


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


def _get_env_int(name, default):
    raw = os.getenv(name)
    if raw is None:
        return int(default)
    try:
        return int(raw, 10)
    except ValueError:
        return int(default)


def _get_env_float(name, default):
    raw = os.getenv(name)
    if raw is None:
        return float(default)
    try:
        return float(raw)
    except ValueError:
        return float(default)


def _encode_id(magic, unique):
    return struct.pack("!I", magic) + unique


def _decode_id(buf):
    magic = struct.unpack("!I", buf[:4])[0]
    return magic, buf[4:ID_SIZE]


def _encode_address(peer):
    if not peer:
        return b"\x00" * ADDR_SIZE
    try:
        if "%" in peer:
            peer = peer.split("%", 1)[0]
        ip = ipaddress.ip_address(peer)
    except ValueError:
        return b"\x00" * ADDR_SIZE
    if ip.version == 4:
        return b"\x00" * 10 + b"\xff\xff" + ip.packed
    return ip.packed


def _peer_ip(peername):
    if isinstance(peername, tuple) and peername:
        return peername[0]
    return ""


def _new_unique(existing):
    while True:
        value = os.urandom(12)
        if value not in existing and value != b"\x00" * 12:
            return value


class ClientConn:
    __slots__ = ("reader", "writer", "addr", "created")

    def __init__(self, reader, writer, addr):
        self.reader = reader
        self.writer = writer
        self.addr = addr
        self.created = _now()


class HostLinkConn:
    __slots__ = ("reader", "writer", "created")

    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.created = _now()


class Session:
    __slots__ = (
        "session_id",
        "host_reader",
        "host_writer",
        "clients",
        "host_links",
        "link_addresses",
        "created",
        "last_seen",
    )

    def __init__(self, session_id, host_reader, host_writer):
        self.session_id = session_id
        self.host_reader = host_reader
        self.host_writer = host_writer
        self.clients = {}
        self.host_links = {}
        self.link_addresses = {}
        self.created = _now()
        self.last_seen = self.created


class RelayState:
    def __init__(self):
        self.sessions = {}
        self.link_to_session = {}


async def _safe_close(writer):
    if writer is None:
        return
    if writer.is_closing():
        return
    writer.close()
    try:
        await writer.wait_closed()
    except (OSError, RuntimeError):
        return


async def _bridge(client, host_link):
    async def pipe(reader, writer):
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                writer.write(data)
                await writer.drain()
        except (OSError, asyncio.CancelledError):
            return

    task_a = asyncio.create_task(pipe(client.reader, host_link.writer))
    task_b = asyncio.create_task(pipe(host_link.reader, client.writer))
    done, pending = await asyncio.wait(
        [task_a, task_b], return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
        task.cancel()
    await _safe_close(client.writer)
    await _safe_close(host_link.writer)


async def _close_session(state, session, reason):
    state.sessions.pop(session.session_id, None)
    for link_id in list(session.clients.keys()):
        client = session.clients.pop(link_id, None)
        if client:
            await _safe_close(client.writer)
        state.link_to_session.pop(link_id, None)
        session.link_addresses.pop(link_id, None)
    for link_id in list(session.host_links.keys()):
        link = session.host_links.pop(link_id, None)
        if link:
            await _safe_close(link.writer)
        state.link_to_session.pop(link_id, None)
        session.link_addresses.pop(link_id, None)
    await _safe_close(session.host_writer)
    print("session closed: {} ({})".format(session.session_id.hex(), reason))


async def _send_id(writer, magic, unique):
    payload = _encode_id(magic, unique)
    writer.write(payload)
    await writer.drain()


async def _send_addr(writer, link_id, addr):
    payload = _encode_id(MAGIC_ADDR, link_id) + addr
    writer.write(payload)
    await writer.drain()


async def _handle_host_control(state, session):
    session_b64 = base64.b64encode(session.session_id).decode("ascii")
    print(
        "host session ready: {} (base64 {})".format(
            session.session_id.hex(), session_b64
        )
    )
    try:
        while True:
            magic_buf = await session.host_reader.readexactly(4)
            magic = struct.unpack("!I", magic_buf)[0]
            session.last_seen = _now()
            if magic == MAGIC_PING:
                continue
            link_id = await session.host_reader.readexactly(12)
            if magic == MAGIC_ADDR:
                addr = session.link_addresses.get(link_id)
                if addr:
                    await _send_addr(session.host_writer, link_id, addr)
            else:
                continue
    except asyncio.IncompleteReadError:
        pass
    except (OSError, RuntimeError):
        pass
    await _close_session(state, session, "host_disconnected")


async def _maybe_pair(state, session, link_id):
    client = session.clients.get(link_id)
    host_link = session.host_links.get(link_id)
    if not client or not host_link:
        return
    session.clients.pop(link_id, None)
    session.host_links.pop(link_id, None)
    session.link_addresses.pop(link_id, None)
    state.link_to_session.pop(link_id, None)
    asyncio.create_task(_bridge(client, host_link))


async def _handle_client(state, session, reader, writer):
    link_id = _new_unique(state.link_to_session)
    peer_ip = _peer_ip(writer.get_extra_info("peername"))
    addr = _encode_address(peer_ip)
    session.clients[link_id] = ClientConn(reader, writer, addr)
    session.link_addresses[link_id] = addr
    state.link_to_session[link_id] = session.session_id
    try:
        await _send_id(session.host_writer, MAGIC_LINK, link_id)
    except (OSError, RuntimeError):
        session.clients.pop(link_id, None)
        session.link_addresses.pop(link_id, None)
        state.link_to_session.pop(link_id, None)
        await _safe_close(writer)
        return
    await _maybe_pair(state, session, link_id)


async def _handle_host_link(state, session, link_id, reader, writer):
    session.host_links[link_id] = HostLinkConn(reader, writer)
    await _maybe_pair(state, session, link_id)


async def _handle_connection(state, max_sessions, reader, writer):
    try:
        header = await reader.readexactly(ID_SIZE)
    except asyncio.IncompleteReadError:
        await _safe_close(writer)
        return
    except (OSError, RuntimeError):
        await _safe_close(writer)
        return

    magic, unique = _decode_id(header)

    if magic == MAGIC_SESSION:
        if unique == b"\x00" * 12:
            if len(state.sessions) >= max_sessions:
                await _safe_close(writer)
                return
            session_id = _new_unique(state.sessions)
            session = Session(session_id, reader, writer)
            state.sessions[session_id] = session
            try:
                await _send_id(writer, MAGIC_SESSION, session_id)
            except (OSError, RuntimeError):
                await _safe_close(writer)
                state.sessions.pop(session_id, None)
                return
            await _handle_host_control(state, session)
            return

        session = state.sessions.get(unique)
        if not session:
            await _safe_close(writer)
            return
        await _handle_client(state, session, reader, writer)
        return

    if magic == MAGIC_LINK:
        session_id = state.link_to_session.get(unique)
        session = state.sessions.get(session_id) if session_id else None
        if not session:
            await _safe_close(writer)
            return
        await _handle_host_link(state, session, unique, reader, writer)
        return

    await _safe_close(writer)


async def _cleanup_loop(state, pending_ttl):
    while True:
        await asyncio.sleep(1.0)
        now = _now()
        for session in list(state.sessions.values()):
            for link_id in list(session.clients.keys()):
                client = session.clients.get(link_id)
                if not client:
                    continue
                if now - client.created > pending_ttl:
                    session.clients.pop(link_id, None)
                    session.link_addresses.pop(link_id, None)
                    state.link_to_session.pop(link_id, None)
                    await _safe_close(client.writer)
            for link_id in list(session.host_links.keys()):
                link = session.host_links.get(link_id)
                if not link:
                    continue
                if now - link.created > pending_ttl:
                    session.host_links.pop(link_id, None)
                    session.link_addresses.pop(link_id, None)
                    state.link_to_session.pop(link_id, None)
                    await _safe_close(link.writer)


async def main():
    bind_addr = os.getenv("TCP_RELAY_BIND", DEFAULT_BIND)
    port = _get_env_int("TCP_RELAY_PORT", DEFAULT_PORT)
    pending_ttl = _get_env_float("TCP_RELAY_PENDING_TTL", DEFAULT_PENDING_TTL)
    max_sessions = _get_env_int("TCP_RELAY_MAX_SESSIONS", DEFAULT_MAX_SESSIONS)

    state = RelayState()
    server = await asyncio.start_server(
        lambda r, w: _handle_connection(state, max_sessions, r, w),
        bind_addr,
        port,
    )

    addrs = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print("TCP relay listening on {} (pending_ttl={}s)".format(addrs, pending_ttl))

    asyncio.create_task(_cleanup_loop(state, pending_ttl))

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
