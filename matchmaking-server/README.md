# GGPO Rendezvous Server

This is a simple UDP rendezvous server used to exchange peer addresses for
RetroArch GGPO netplay.

Build
- MSVC: `cl /O2 /EHsc rendezvous_server.c /link ws2_32.lib`
- MinGW: `gcc -O2 -o rendezvous_server.exe rendezvous_server.c -lws2_32`
- Linux/macOS: `cc -O2 -o rendezvous_server rendezvous_server.c`

Run
`rendezvous_server [port]`

The default port is 7000. Each client sends `RNDV1 H <room>` (host) or
`RNDV1 C <room>` (client). The server replies with:
- `WAIT <room>` when no peer is available
- `PEER <ip> <port>` when both host and client are present

RetroArch settings:
- Enable `netplay_use_rendezvous`
- Set `netplay_rendezvous_server`, `netplay_rendezvous_port`, and
  `netplay_rendezvous_room`
