/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "sync.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct PerfState {
   std::vector<byte> data;
   uint32 rng;
};

static PerfState g_state;

static void MutateState()
{
   uint32 x = g_state.rng;
   if (g_state.data.empty()) {
      return;
   }
   for (size_t i = 0; i < g_state.data.size(); i += 64) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      g_state.data[i] = (byte)(x & 0xFF);
   }
   g_state.rng = x;
}

static bool __cdecl BeginGame(const char *game)
{
   (void)game;
   return true;
}

static bool __cdecl SaveGameState(unsigned char **buffer, int *len, int *checksum, int frame)
{
   (void)frame;
   if (g_state.data.empty()) {
      return false;
   }
   if (g_state.data.size() > (size_t)INT_MAX) {
      return false;
   }

   int size = (int)g_state.data.size();
   unsigned char *copy = (unsigned char *)malloc(size);
   if (!copy) {
      return false;
   }
   memcpy(copy, &g_state.data[0], size);

   *buffer = copy;
   *len = size;
   if (checksum) {
      *checksum = 0;
   }
   return true;
}

static bool __cdecl LoadGameState(unsigned char *buffer, int len)
{
   if (!buffer || len <= 0 || g_state.data.empty()) {
      return false;
   }

   size_t size = (size_t)len;
   if (size > g_state.data.size()) {
      size = g_state.data.size();
   }
   memcpy(&g_state.data[0], buffer, size);
   return true;
}

static bool __cdecl LogGameState(char *filename, unsigned char *buffer, int len)
{
   (void)filename;
   (void)buffer;
   (void)len;
   return true;
}

static void __cdecl FreeBuffer(void *buffer)
{
   free(buffer);
}

static bool __cdecl AdvanceFrame(int flags)
{
   (void)flags;
   MutateState();
   return true;
}

static bool __cdecl OnEvent(GGPOEvent *info)
{
   (void)info;
   return true;
}

struct PerfConfig {
   int state_kb;
   int frames;
   int loads;
   int lz4_accel;
   bool show_help;
};

static void PrintUsage(const char *exe)
{
   printf("Usage: %s [options]\n", exe);
   printf("  --state-kb=NN   State size in KB (default 256)\n");
   printf("  --frames=NN     Number of saved frames (default 2000)\n");
   printf("  --loads=NN      Number of load operations (default 2000)\n");
   printf("  --lz4-accel=NN  LZ4 acceleration (default 2)\n");
   printf("  -h, --help      Show this help\n");
}

static PerfConfig ParseArgs(int argc, char **argv)
{
   PerfConfig config;
   config.state_kb = 256;
   config.frames = 2000;
   config.loads = 2000;
   config.lz4_accel = 2;
   config.show_help = false;

   for (int i = 1; i < argc; ++i) {
      const char *arg = argv[i];
      if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
         config.show_help = true;
         continue;
      }
      if (!strncmp(arg, "--state-kb=", 11)) {
         config.state_kb = atoi(arg + 11);
         continue;
      }
      if (!strncmp(arg, "--frames=", 9)) {
         config.frames = atoi(arg + 9);
         continue;
      }
      if (!strncmp(arg, "--loads=", 8)) {
         config.loads = atoi(arg + 8);
         continue;
      }
      if (!strncmp(arg, "--lz4-accel=", 12)) {
         config.lz4_accel = atoi(arg + 12);
         continue;
      }
   }

   if (config.state_kb <= 0) {
      config.state_kb = 256;
   }
   if (config.frames < 2) {
      config.frames = 2;
   }
   if (config.loads < 1) {
      config.loads = 1;
   }
   if (config.lz4_accel <= 0) {
      config.lz4_accel = 2;
   }

   return config;
}

class PerfSync : public Sync {
public:
   explicit PerfSync(UdpMsg::connect_status *connect_status) : Sync(connect_status) {}

   void SetFrameCount(int frame) { _framecount = frame; }
   void SaveFrame() { SaveCurrentFrame(); }

   void LoadFrameForPerf(int frame)
   {
      int saved_head = _savedstate.head;
      int saved_frame = _framecount;
      LoadFrame(frame);
      _savedstate.head = saved_head;
      _framecount = saved_frame;
   }

   void GetLastFrameStats(int *uncompressed_size, int *compressed_size, bool *compressed) const
   {
      int index = _savedstate.head - 1;
      if (index < 0) {
         index = ARRAY_SIZE(_savedstate.frames) - 1;
      }
      const SavedFrame &frame = _savedstate.frames[index];
      if (uncompressed_size) {
         *uncompressed_size = frame.uncompressed_size;
      }
      if (compressed_size) {
         *compressed_size = frame.cbuf;
      }
      if (compressed) {
         *compressed = frame.compressed;
      }
   }
};

int main(int argc, char **argv)
{
   PerfConfig cfg = ParseArgs(argc, argv);
   if (cfg.show_help) {
      PrintUsage(argv[0]);
      return 0;
   }

   size_t state_size = (size_t)cfg.state_kb * 1024u;
   if (state_size == 0 || state_size > (size_t)INT_MAX) {
      printf("Invalid state size.\n");
      return 1;
   }

   g_state.data.assign(state_size, 0);
   g_state.rng = 0x12345678u;

   UdpMsg::connect_status connect_status[UDP_MSG_MAX_PLAYERS];
   memset(connect_status, 0, sizeof(connect_status));

   GGPOSessionCallbacks callbacks;
   memset(&callbacks, 0, sizeof(callbacks));
   callbacks.begin_game = BeginGame;
   callbacks.save_game_state = SaveGameState;
   callbacks.load_game_state = LoadGameState;
   callbacks.log_game_state = LogGameState;
   callbacks.free_buffer = FreeBuffer;
   callbacks.advance_frame = AdvanceFrame;
   callbacks.on_event = OnEvent;

   Sync::Config config = { 0 };
   config.callbacks = callbacks;
   config.num_players = 2;
   config.input_size = 4;
   config.num_prediction_frames = MAX_PREDICTION_FRAMES;
   config.lz4_accel = cfg.lz4_accel;

   PerfSync sync(connect_status);
   sync.Init(config);

   unsigned long long total_uncompressed = 0;
   unsigned long long total_compressed = 0;
   int compressed_frames = 0;

   uint32 save_start = Platform::GetCurrentTimeMS();
   for (int i = 0; i < cfg.frames; ++i) {
      MutateState();
      sync.SetFrameCount(i);
      sync.SaveFrame();
      int uncompressed_size = 0;
      int compressed_size = 0;
      bool compressed = false;
      sync.GetLastFrameStats(&uncompressed_size, &compressed_size, &compressed);
      total_uncompressed += (unsigned long long)uncompressed_size;
      total_compressed += (unsigned long long)compressed_size;
      if (compressed) {
         compressed_frames++;
      }
   }
   int save_ms = (int)(Platform::GetCurrentTimeMS() - save_start);
   if (save_ms <= 0) {
      save_ms = 1;
   }

   int current_frame = cfg.frames - 1;
   int ring_size = MAX_PREDICTION_FRAMES + 2;
   int oldest_frame = current_frame - (ring_size - 1);
   if (oldest_frame < 0) {
      oldest_frame = 0;
   }
   int load_span = current_frame - oldest_frame;
   int load_ms = 0;

   if (load_span > 0) {
      uint32 load_start = Platform::GetCurrentTimeMS();
      for (int i = 0; i < cfg.loads; ++i) {
         int offset = 1 + (i % load_span);
         int target = current_frame - offset;
         sync.LoadFrameForPerf(target);
      }
      load_ms = (int)(Platform::GetCurrentTimeMS() - load_start);
      if (load_ms <= 0) {
         load_ms = 1;
      }
   }

   double save_fps = (double)cfg.frames * 1000.0 / (double)save_ms;
   double load_fps = 0.0;
   if (load_span > 0) {
      load_fps = (double)cfg.loads * 1000.0 / (double)load_ms;
   }

   double avg_ratio = 0.0;
   if (total_uncompressed > 0) {
      avg_ratio = (double)total_compressed / (double)total_uncompressed;
   }

   printf("GGPO Sync Perf Harness\n");
   printf("State: %d KB, frames: %d, loads: %d, lz4_accel: %d\n",
          cfg.state_kb, cfg.frames, cfg.loads, cfg.lz4_accel);
   printf("Save: %d frames in %d ms (%.1f fps)\n", cfg.frames, save_ms, save_fps);
   if (load_span > 0) {
      printf("Load: %d loads in %d ms (%.1f fps)\n", cfg.loads, load_ms, load_fps);
   } else {
      printf("Load: skipped (not enough saved frames)\n");
   }
   printf("Compression: %d/%d frames compressed, avg %.1f%%\n",
          compressed_frames, cfg.frames, avg_ratio * 100.0);
   printf("Avg sizes: %llu -> %llu bytes\n",
          (unsigned long long)(total_uncompressed / (unsigned long long)cfg.frames),
          (unsigned long long)(total_compressed / (unsigned long long)cfg.frames));

   return 0;
}
