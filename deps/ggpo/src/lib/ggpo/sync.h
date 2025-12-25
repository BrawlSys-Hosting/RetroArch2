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
      int      frame;
      int      checksum;
      bool     compressed;
      bool     delta;
      SavedFrame() : buf(NULL), cbuf(0), uncompressed_size(0), frame(-1), checksum(0),
         compressed(false), delta(false) { }
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

   void LoadFrame(int frame);
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
   std::vector<byte>       _decompress_buffer;
   std::vector<byte>       _delta_buffer;
   std::vector<byte>       _last_state;
   int                     _last_state_size;
   int                     _last_state_frame;
   bool                    _last_state_valid;
   DeltaStats              _delta_stats;

   RingBuffer<Event, 32> _event_queue;
   UdpMsg::connect_status *_local_connect_status;
};

#endif

