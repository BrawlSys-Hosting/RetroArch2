#!/usr/bin/env python3
"""Simple GGPO rendezvous server for RetroArch (UDP, ASCII)."""

import logging
import os
import socket
import sys
import time

DEFAULT_PORT = 7000
MAX_ROOMS = 128
ROOM_NAME_MAX = 64
BUF_SIZE = 256
MAGIC = "RNDV1"
ROOM_TIMEOUT_SEC = 30
PEER_BURST_COUNT = 3

logger = logging.getLogger("rendezvous")


def setup_logging():
    level_name = os.getenv("RENDEZVOUS_LOG_LEVEL", "INFO").upper()
    level = getattr(logging, level_name, logging.INFO)
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(message)s")
    logger.setLevel(level)
    return logger


def find_or_create_room(rooms, name):
    for room in rooms:
        if room["name"] == name:
            return room, False
    if len(rooms) >= MAX_ROOMS:
        return None, False
    room = {
        "name": name,
        "has_host": False,
        "has_client": False,
        "host_addr": None,
        "client_addr": None,
        "host_seen": 0,
        "client_seen": 0,
    }
    rooms.append(room)
    return room, True


def prune_rooms(rooms):
    now = int(time.time())
    i = 0
    while i < len(rooms):
        room = rooms[i]
        if room["has_host"] and (now - room["host_seen"]) > ROOM_TIMEOUT_SEC:
            logger.info("Room %s host timed out.", room["name"])
            room["has_host"] = False
        if room["has_client"] and (now - room["client_seen"]) > ROOM_TIMEOUT_SEC:
            logger.info("Room %s client timed out.", room["name"])
            room["has_client"] = False
        if not room["has_host"] and not room["has_client"]:
            logger.info("Room %s removed.", room["name"])
            rooms.pop(i)
            continue
        i += 1


def send_wait(sock, to_addr, room):
    msg = f"WAIT {room}"
    sock.sendto(msg.encode("ascii", "replace"), to_addr)


def send_peer(sock, to_addr, peer_addr):
    ip, port = peer_addr
    msg = f"PEER {ip} {port}"
    sock.sendto(msg.encode("ascii", "replace"), to_addr)


def send_peer_burst(sock, to_addr, peer_addr):
    for _ in range(PEER_BURST_COUNT):
        send_peer(sock, to_addr, peer_addr)


def parse_port(argv):
    if len(argv) < 2:
        return DEFAULT_PORT
    try:
        port = int(argv[1], 10)
    except ValueError:
        raise ValueError(f"Invalid port: {argv[1]}")
    if port < 1 or port > 65535:
        raise ValueError(f"Invalid port: {port}")
    return port


def main(argv):
    setup_logging()
    try:
        port = parse_port(argv)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))

    logger.info("GGPO rendezvous server listening on UDP %s", port)

    rooms = []
    while True:
        data, addr = sock.recvfrom(BUF_SIZE)
        if not data:
            continue
        text = data.decode("utf-8", "replace").strip()
        logger.debug("Recv %s:%s '%s'", addr[0], addr[1], text)
        parts = text.split()
        if len(parts) < 3:
            logger.debug("Dropping malformed packet from %s:%s", addr[0], addr[1])
            continue
        magic, role_token, room_name = parts[0], parts[1], parts[2]
        if magic != MAGIC:
            logger.debug("Dropping packet with bad magic from %s:%s", addr[0], addr[1])
            continue
        role = role_token[:1]
        room_name = room_name[: ROOM_NAME_MAX - 1]
        if not room_name:
            continue

        prune_rooms(rooms)
        room, created = find_or_create_room(rooms, room_name)
        if room is None:
            logger.warning("Room limit reached; dropping room %s.", room_name)
            continue
        if created:
            logger.info("Room %s created.", room_name)

        now = int(time.time())
        if role == "H":
            room["host_addr"] = addr
            room["host_seen"] = now
            room["has_host"] = True
            logger.info("Room %s host registered at %s:%s.", room_name, addr[0], addr[1])
        elif role == "C":
            room["client_addr"] = addr
            room["client_seen"] = now
            room["has_client"] = True
            logger.info("Room %s client registered at %s:%s.", room_name, addr[0], addr[1])
        else:
            logger.debug("Dropping packet with unknown role from %s:%s", addr[0], addr[1])
            continue

        if room["has_host"] and room["has_client"]:
            logger.info(
                "Room %s exchanging peers host=%s:%s client=%s:%s.",
                room_name,
                room["host_addr"][0],
                room["host_addr"][1],
                room["client_addr"][0],
                room["client_addr"][1],
            )
            send_peer_burst(sock, room["host_addr"], room["client_addr"])
            send_peer_burst(sock, room["client_addr"], room["host_addr"])
        else:
            logger.debug("Room %s waiting for peer.", room_name)
            send_wait(sock, addr, room_name)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
