# RetroArch Lobby Server

This is a lightweight HTTP lobby server for RetroArch netplay. It supports room
announcements (`/add`), room listings (`/list`), and MITM tunnel lookups
(`/tunnel`). Rooms are stored in memory and expire after a TTL.

## Run

```sh
python lobby_server.py
```

Environment variables (loaded from `.env` if present):

- `LOBBY_BIND` (default `0.0.0.0`)
- `LOBBY_PORT` (default `55435`)
- `LOBBY_ROOM_TTL` (default `180` seconds)
- `LOBBY_MAX_ROOMS` (default `512`)
- `LOBBY_MITM_CONFIG` (default `mitm_servers.json`)
- `LOBBY_TRUST_PROXY` (default unset; set to `1` to honor `X-Forwarded-For`)
- `LOBBY_PUBLIC_IP` (optional public IP/hostname to replace private/loopback)
- `LOBBY_PUBLIC_RENDEZVOUS` (optional public rendezvous host override)

Example:

```sh
set LOBBY_BIND=0.0.0.0
set LOBBY_PORT=55435
python lobby_server.py
```

## Endpoints

- `POST /add` - accepts `application/x-www-form-urlencoded` from RetroArch and
  returns a key/value response with the assigned room id.
- `GET /list` - returns JSON rooms in the format RetroArch expects.
- `GET /tunnel?name=<handle>` - returns `tunnel_addr` and `tunnel_port` for the
  MITM server handle.

## GGPO Relay Fields

If RetroArch announces GGPO relay settings, this server stores and returns:

- `ggpo_relay` (0/1)
- `ggpo_relay_server`
- `ggpo_relay_port`
- `ggpo_relay_session`

These fields are included in both the `/add` response and `/list` JSON so
clients can surface relay-enabled rooms and auto-apply relay settings.

## MITM Server Mapping

Create `mitm_servers.json` next to `lobby_server.py` (or point
`LOBBY_MITM_CONFIG` at it):

```json
{
  "nyc": {"addr": "203.0.113.10", "port": 55435},
  "custom": {"addr": "127.0.0.1", "port": 55435}
}
```

## RetroArch Integration

RetroArch uses `FILE_PATH_LOBBY_LIBRETRO_URL` (in `file_path_special.h`) as the
base URL for `/add`, `/list`, and `/tunnel`. To use this server, either:

- change `FILE_PATH_LOBBY_LIBRETRO_URL` to your server URL and rebuild, or
- override DNS/hosts so `lobby.libretro.com` resolves to this server.
