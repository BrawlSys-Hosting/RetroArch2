# RetroArch GGPO Relay Server

This is a lightweight UDP relay for GGPO traffic. It pairs two clients by
session id and forwards all non-control UDP packets between them.

The relay expects a simple ASCII control message to register clients before
starting GGPO.

## Run

```sh
python relay_server.py
```

Environment variables (loaded from `.env` if present):

- `RELAY_BIND` (default `0.0.0.0`)
- `RELAY_PORT` (default `7001`)
- `RELAY_SESSION_TTL` (default `120` seconds)
- `RELAY_CLIENT_TTL` (default `30` seconds)
- `RELAY_MAX_SESSIONS` (default `512`)
- `RELAY_MAX_PACKET` (default `2048` bytes)

## Protocol

All control messages start with the ASCII prefix `RARELAY1` and are sent over
UDP to the relay port.

Register:

```
RARELAY1 HELLO <session_id> [slot]
```

- `session_id` is an arbitrary string used to pair two clients.
- `slot` is optional and must be `1` or `2`. If omitted, the relay assigns the
  first available slot.

Responses:

- `RARELAY1 WAIT <session_id> <slot>` - registered, waiting for peer.
- `RARELAY1 READY <session_id> <slot>` - peer connected, relay active.
- `RARELAY1 BUSY <session_id>` - requested slot is taken.
- `RARELAY1 FULL <session_id>` - server has no capacity for new sessions.
- `RARELAY1 ERR <reason>` - invalid request.

Leave:

```
RARELAY1 BYE <session_id>
```

Keepalive:

```
RARELAY1 PING <session_id>
```

Once both peers are registered, the relay forwards all other UDP packets
between the two addresses verbatim. This allows GGPO traffic to traverse NAT
via the relay.

## RetroArch Integration

RetroArch GGPO currently expects a direct peer address. To use this relay, the
client must send the `HELLO` message first and then direct GGPO traffic to the
relay server address/port. Client-side changes are required to automate this
for the GGPO backend.
