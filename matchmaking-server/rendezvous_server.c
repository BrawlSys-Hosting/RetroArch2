/* Simple GGPO rendezvous server for RetroArch.
 * Protocol (UDP, ASCII):
 *  Client -> server: "RNDV1 H <room>" or "RNDV1 C <room>"
 *  Server -> client: "WAIT <room>" or "PEER <ip> <port>"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

#define DEFAULT_PORT 7000
#define MAX_ROOMS 128
#define ROOM_NAME_MAX 64
#define BUF_SIZE 256
#define MAGIC "RNDV1"
#define ROOM_TIMEOUT_SEC 30

#ifdef _WIN32
typedef SOCKET socket_t;
#define INVALID_SOCKET_FD INVALID_SOCKET
static void close_socket(socket_t fd)
{
   closesocket(fd);
}
#else
typedef int socket_t;
#define INVALID_SOCKET_FD (-1)
static void close_socket(socket_t fd)
{
   close(fd);
}
#endif

typedef struct room_entry
{
   char name[ROOM_NAME_MAX];
   int has_host;
   int has_client;
   struct sockaddr_in host_addr;
   struct sockaddr_in client_addr;
   time_t host_seen;
   time_t client_seen;
} room_entry_t;

static room_entry_t rooms[MAX_ROOMS];
static size_t room_count = 0;

static room_entry_t *find_or_create_room(const char *name)
{
   size_t i;

   for (i = 0; i < room_count; i++)
   {
      if (strcmp(rooms[i].name, name) == 0)
         return &rooms[i];
   }

   if (room_count >= MAX_ROOMS)
      return NULL;

   memset(&rooms[room_count], 0, sizeof(rooms[room_count]));
   snprintf(rooms[room_count].name, sizeof(rooms[room_count].name), "%s", name);
   return &rooms[room_count++];
}

static void prune_rooms(void)
{
   size_t i = 0;
   time_t now = time(NULL);

   while (i < room_count)
   {
      room_entry_t *room = &rooms[i];

      if (room->has_host && (now - room->host_seen) > ROOM_TIMEOUT_SEC)
         room->has_host = 0;
      if (room->has_client && (now - room->client_seen) > ROOM_TIMEOUT_SEC)
         room->has_client = 0;

      if (!room->has_host && !room->has_client)
      {
         if (i + 1 < room_count)
            memmove(&rooms[i], &rooms[i + 1],
                  (room_count - i - 1) * sizeof(*rooms));
         room_count--;
         continue;
      }

      i++;
   }
}

static void send_wait(socket_t sock, const struct sockaddr_in *to,
      const char *room)
{
   char msg[BUF_SIZE];
   int len;

   len = snprintf(msg, sizeof(msg), "WAIT %s", room);
   if (len < 0)
      return;
   if (len >= (int)sizeof(msg))
      len = (int)sizeof(msg) - 1;
   msg[len] = '\0';

   sendto(sock, msg, (int)strlen(msg), 0,
         (const struct sockaddr*)to, sizeof(*to));
}

static void send_peer(socket_t sock, const struct sockaddr_in *to,
      const struct sockaddr_in *peer)
{
   char msg[BUF_SIZE];
   char ip[INET_ADDRSTRLEN];
   unsigned short port = ntohs(peer->sin_port);
   int len;

   if (!inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip)))
      return;

   len = snprintf(msg, sizeof(msg), "PEER %s %u", ip, (unsigned)port);
   if (len < 0)
      return;
   if (len >= (int)sizeof(msg))
      len = (int)sizeof(msg) - 1;
   msg[len] = '\0';

   sendto(sock, msg, (int)strlen(msg), 0,
         (const struct sockaddr*)to, sizeof(*to));
}

int main(int argc, char **argv)
{
   socket_t sock = INVALID_SOCKET_FD;
   struct sockaddr_in addr;
   unsigned port = DEFAULT_PORT;

#ifdef _WIN32
   WSADATA wsa_data;
   if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
      return 1;
#endif

   if (argc > 1)
   {
      port = (unsigned)strtoul(argv[1], NULL, 10);
      if (port > 65535)
      {
         fprintf(stderr, "Invalid port: %u\n", port);
         return 1;
      }
   }

   sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (sock == INVALID_SOCKET_FD)
   {
      fprintf(stderr, "Failed to create UDP socket.\n");
      return 1;
   }

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   addr.sin_port = htons((unsigned short)port);

   if (bind(sock, (const struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
      fprintf(stderr, "Failed to bind UDP port %u.\n", port);
      close_socket(sock);
      return 1;
   }

   printf("GGPO rendezvous server listening on UDP %u\n", port);

   for (;;)
   {
      struct sockaddr_in from;
      socklen_t from_len = sizeof(from);
      char buf[BUF_SIZE];
      char magic[6];
      char role = '\0';
      char room_name[ROOM_NAME_MAX];
      int recvd;
      time_t now;
      room_entry_t *room;

      memset(&from, 0, sizeof(from));
      recvd = (int)recvfrom(sock, buf, sizeof(buf) - 1, 0,
            (struct sockaddr*)&from, &from_len);
      if (recvd <= 0)
         continue;

      buf[recvd] = '\0';

      if (sscanf(buf, "%5s %c %63s", magic, &role, room_name) != 3)
         continue;
      if (strcmp(magic, MAGIC) != 0)
         continue;

      prune_rooms();
      room = find_or_create_room(room_name);
      if (!room)
         continue;

      now = time(NULL);
      if (role == 'H')
      {
         room->host_addr = from;
         room->host_seen = now;
         room->has_host  = 1;
      }
      else if (role == 'C')
      {
         room->client_addr = from;
         room->client_seen = now;
         room->has_client  = 1;
      }
      else
      {
         continue;
      }

      if (room->has_host && room->has_client)
      {
         send_peer(sock, &room->host_addr, &room->client_addr);
         send_peer(sock, &room->client_addr, &room->host_addr);
      }
      else
      {
         send_wait(sock, &from, room_name);
      }
   }

   close_socket(sock);

#ifdef _WIN32
   WSACleanup();
#endif

   return 0;
}
