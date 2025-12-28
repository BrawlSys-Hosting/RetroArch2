/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _SYNC_H
#define _SYNC_H

#include "types.h"
#include "ggponet.h"
#include "game_input.h"
#include "input_queue.h"
#include "ring_buffer.h"
#include "network/udp_msg.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define MAX_PREDICTION_FRAMES    8
#define GGPO_STATE_KEYFRAME_INTERVAL 4

class SyncTestBackend;

class Sync {
public:
   struct Config {
      GGPOSessionCallbacks    callbacks;
      int                     num_prediction_frames;
      int                     num_players;
      int                     input_size;
      int                     lz4_accel;
      int                     async_compress;
   };
   struct Event {
      enum {
         ConfirmedInput,
      } type;
      union {
         struct {
            GameInput   input;
         } confirmedInput;
      } u;
   };

public:
   Sync(UdpMsg::connect_status *connect_status);
   virtual ~Sync();

   void Init(Config &config);

   void SetLastConfirmedFrame(int frame);
   void SetFrameDelay(int queue, int delay);
   bool AddLocalInput(int queue, GameInput &input);
   void AddRemoteInput(int queue, GameInput &input);
   int GetConfirmedInputs(void *values, int size, int frame);
   int SynchronizeInputs(void *values, int size);

   void CheckSimulation(int timeout);
   void AdjustSimulation(int seek_to);
   void IncrementFrame(void);

   int GetFrameCount() { return _framecount; }
   bool InRollback() { return _rollingback; }

   bool GetEvent(Event &e);
   void GetStateStats(GGPOStateStats *stats);

protected:
   friend SyncTestBackend;

   struct SavedFrame {
      byte    *buf;
      int      cbuf;
      int      uncompressed_size;
      int      buf_capacity;
      int      frame;
      int      checksum;
      bool     compressed;
      bool     delta;
      bool     compress_pending;
      SavedFrame() : buf(NULL), cbuf(0), uncompressed_size(0), buf_capacity(0), frame(-1), checksum(0),
         compressed(false), delta(false), compress_pending(false) { }
   };
   struct ScratchBuffer {
      byte    *data;
      int      size;
      int      capacity;
      ScratchBuffer() : data(NULL), size(0), capacity(0) { }
   };
   struct SavedState {
      SavedFrame frames[MAX_PREDICTION_FRAMES + 2];
      int head;
   };

   struct DeltaStats {
      unsigned long long delta_bytes_sum;
      unsigned long long delta_raw_bytes_sum;
      int delta_frames;
      int keyframes;
      int delta_ratio_last;
      int delta_ratio_max;
      DeltaStats()
         : delta_bytes_sum(0)
         , delta_raw_bytes_sum(0)
         , delta_frames(0)
         , keyframes(0)
         , delta_ratio_last(0)
         , delta_ratio_max(0)
      {}
   };

   bool LoadFrame(int frame);
   void SaveCurrentFrame();
   int FindSavedFrameIndex(int frame);
   SavedFrame &GetLastSavedFrame();
   void FreeSavedFrameBuffer(SavedFrame &state);
   bool DecodeSavedFrame(const SavedFrame &state, std::vector<byte> &buffer);
   bool ReconstructFrame(int frame, std::vector<byte> &buffer);
   void UpdateLastState(const byte *state, int size, int frame);

   bool CreateQueues(Config &config);
   bool CheckSimulationConsistency(int *seekTo);
   void ResetPrediction(int frameNumber);

private:
   struct CompressJob {
      SavedFrame  *state;
      const byte  *input;
      int          input_size;
      int          frame;
      int          lz4_accel;
   };

   struct CompressResult {
      SavedFrame  *state;
      const byte  *input;
      int          input_size;
      int          frame;
      char        *compressed_buf;
      int          compressed_size;
   };

   void StartCompressionThread();
   void StopCompressionThread();
   void CompressionThreadMain();
   bool QueueCompression(SavedFrame *state, const byte *input, int input_size);
   void ProcessCompressionResults();
   void ApplyCompressionResult(const CompressResult &result);
   void WaitForCompression(SavedFrame &state);
   void CompressSync(SavedFrame &state, const byte *input, int input_size);
   byte *AcquireStateBuffer(int *capacity);
   void RecycleStateBuffer(byte *buffer, int capacity);
   void ClearStateBufferPool();
   void ResetScratchBuffer(ScratchBuffer &buffer);
   void FreeScratchBuffer(ScratchBuffer &buffer);
   void EnsureScratchBufferSize(ScratchBuffer &buffer, int size);
   bool DecodeSavedFrameInternal(const SavedFrame &state, ScratchBuffer &buffer);
   bool DecodeSavedFrameRaw(const SavedFrame &state, byte *buffer, int buffer_size);
   bool ReconstructFrameInternal(int frame, ScratchBuffer &buffer);

   bool _async_compress;
   std::thread _compress_thread;
   std::mutex _compress_mutex;
   std::condition_variable _compress_cv;
   std::condition_variable _compress_done_cv;
   bool _compress_shutdown;
   std::deque<CompressJob> _compress_jobs;
   std::deque<CompressResult> _compress_results;
   int _compress_jobs_max;
   int _compress_results_max;
   struct StateBuffer {
      byte *buffer;
      int capacity;
   };
   std::vector<StateBuffer> _state_buffer_pool;
   int _state_buffer_size_hint;

protected:
   GGPOSessionCallbacks _callbacks;
   SavedState     _savedstate;
   Config         _config;

   bool           _rollingback;
   int            _last_confirmed_frame;
   int            _framecount;
   int            _max_prediction_frames;

   int            _lz4_accel;

   std::vector<InputQueue> _input_queues;
   ScratchBuffer           _decompress_buffer;
   ScratchBuffer           _delta_buffer;
   ScratchBuffer           _last_state;
   int                     _last_state_size;
   int                     _last_state_frame;
   bool                    _last_state_valid;
   DeltaStats              _delta_stats;

   RingBuffer<Event, 32> _event_queue;
   UdpMsg::connect_status *_local_connect_status;
};

#endif

