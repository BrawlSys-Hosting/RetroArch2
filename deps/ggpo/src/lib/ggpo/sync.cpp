/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "sync.h"

#include <stdlib.h>
#include <string.h>

#include "lz4.h"

Sync::Sync(UdpMsg::connect_status *connect_status) :
 _local_connect_status(connect_status)
{
   _framecount = 0;
   _last_confirmed_frame = -1;
   _max_prediction_frames = 0;
   _lz4_accel = 1;
   _last_state_size = 0;
   _last_state_frame = -1;
   _last_state_valid = false;
   memset(&_savedstate, 0, sizeof(_savedstate));
}

Sync::~Sync()
{
   /*
    * Delete frames manually here rather than in a destructor of the SavedFrame
    * structure so we can efficently copy frames via weak references.
    */
   for (int i = 0; i < ARRAY_SIZE(_savedstate.frames); i++) {
      FreeSavedFrameBuffer(_savedstate.frames[i]);
   }
}

void
Sync::Init(Sync::Config &config)
{
   _config = config;
   _callbacks = config.callbacks;
   _framecount = 0;
   _rollingback = false;
   _last_state_valid = false;
   _last_state_size = 0;
   _last_state_frame = -1;
   _last_state.clear();
   _delta_buffer.clear();
   _delta_stats = DeltaStats();

   _max_prediction_frames = config.num_prediction_frames;
   _lz4_accel = config.lz4_accel;
   if (_lz4_accel <= 0) {
      _lz4_accel = Platform::GetConfigInt("ggpo.sync.lz4_accel");
      if (_lz4_accel <= 0) {
         _lz4_accel = 2;
      }
   }

   CreateQueues(config);
}

void
Sync::SetLastConfirmedFrame(int frame) 
{   
   _last_confirmed_frame = frame;
   if (_last_confirmed_frame > 0) {
      for (int i = 0; i < _config.num_players; i++) {
         _input_queues[i].DiscardConfirmedFrames(frame - 1);
      }
   }
}

bool
Sync::AddLocalInput(int queue, GameInput &input)
{
   int frames_behind = _framecount - _last_confirmed_frame; 
   if (_framecount >= _max_prediction_frames && frames_behind >= _max_prediction_frames) {
      Log("Rejecting input from emulator: reached prediction barrier.\n");
      return false;
   }

   if (_framecount == 0) {
      SaveCurrentFrame();
   }

   Log("Sending undelayed local frame %d to queue %d.\n", _framecount, queue);
   input.frame = _framecount;
   _input_queues[queue].AddInput(input);

   return true;
}

void
Sync::AddRemoteInput(int queue, GameInput &input)
{
   _input_queues[queue].AddInput(input);
}

int
Sync::GetConfirmedInputs(void *values, int size, int frame)
{
   int disconnect_flags = 0;
   char *output = (char *)values;

   ASSERT(size >= _config.num_players * _config.input_size);

   memset(output, 0, size);
   for (int i = 0; i < _config.num_players; i++) {
      GameInput input;
      if (_local_connect_status[i].disconnected && frame > _local_connect_status[i].last_frame) {
         disconnect_flags |= (1 << i);
         input.erase();
      } else {
         _input_queues[i].GetConfirmedInput(frame, &input);
      }
      memcpy(output + (i * _config.input_size), input.bits, _config.input_size);
   }
   return disconnect_flags;
}

int
Sync::SynchronizeInputs(void *values, int size)
{
   int disconnect_flags = 0;
   char *output = (char *)values;

   ASSERT(size >= _config.num_players * _config.input_size);

   memset(output, 0, size);
   for (int i = 0; i < _config.num_players; i++) {
      GameInput input;
      if (_local_connect_status[i].disconnected && _framecount > _local_connect_status[i].last_frame) {
         disconnect_flags |= (1 << i);
         input.erase();
      } else {
         _input_queues[i].GetInput(_framecount, &input);
      }
      memcpy(output + (i * _config.input_size), input.bits, _config.input_size);
   }
   return disconnect_flags;
}

void
Sync::CheckSimulation(int timeout)
{
   int seek_to;
   if (!CheckSimulationConsistency(&seek_to)) {
      AdjustSimulation(seek_to);
   }
}

void
Sync::IncrementFrame(void)
{
   _framecount++;
   SaveCurrentFrame();
}

void
Sync::AdjustSimulation(int seek_to)
{
   int framecount = _framecount;
   int count = _framecount - seek_to;

   Log("Catching up\n");
   _rollingback = true;

   /*
    * Flush our input queue and load the last frame.
    */
   LoadFrame(seek_to);
   ASSERT(_framecount == seek_to);

   /*
    * Advance frame by frame (stuffing notifications back to 
    * the master).
    */
   ResetPrediction(_framecount);
   for (int i = 0; i < count; i++) {
      _callbacks.advance_frame(0);
   }
   ASSERT(_framecount == framecount);

   _rollingback = false;

   Log("---\n");   
}

void
Sync::UpdateLastState(const byte *state, int size, int frame)
{
   if (!state || size <= 0) {
      _last_state_valid = false;
      _last_state_size = 0;
      _last_state_frame = -1;
      _last_state.clear();
      return;
   }

   if (_last_state.size() < (size_t)size) {
      _last_state.resize(size);
   }
   memcpy(&_last_state[0], state, size);
   _last_state_size = size;
   _last_state_frame = frame;
   _last_state_valid = true;
}

bool
Sync::DecodeSavedFrame(const SavedFrame &state, std::vector<byte> &buffer)
{
   if (!state.buf || state.uncompressed_size <= 0) {
      return false;
   }

   if (buffer.size() < (size_t)state.uncompressed_size) {
      buffer.resize(state.uncompressed_size);
   }

   if (state.compressed) {
      int decoded = LZ4_decompress_safe((const char *)state.buf,
                                        (char *)&buffer[0],
                                        state.cbuf,
                                        state.uncompressed_size);
      return decoded == state.uncompressed_size;
   }

   memcpy(&buffer[0], state.buf, state.uncompressed_size);
   return true;
}

bool
Sync::ReconstructFrame(int frame, std::vector<byte> &buffer)
{
   int base_frame = frame;
   bool found_base = false;
   SavedFrame *state = _savedstate.frames + FindSavedFrameIndex(frame);

   if (!state->delta) {
      return DecodeSavedFrame(*state, buffer);
   }

   while (base_frame >= 0) {
      SavedFrame *base_state = _savedstate.frames + FindSavedFrameIndex(base_frame);
      if (!base_state->delta) {
         if (!DecodeSavedFrame(*base_state, buffer)) {
            return false;
         }
         found_base = true;
         break;
      }
      base_frame--;
   }

   if (!found_base) {
      return false;
   }

   for (int f = base_frame + 1; f <= frame; f++) {
      SavedFrame *delta_state = _savedstate.frames + FindSavedFrameIndex(f);
      if (!delta_state->delta) {
         if (!DecodeSavedFrame(*delta_state, buffer)) {
            return false;
         }
         continue;
      }

      if (_delta_buffer.size() < (size_t)delta_state->uncompressed_size) {
         _delta_buffer.resize(delta_state->uncompressed_size);
      }

      if (!DecodeSavedFrame(*delta_state, _delta_buffer)) {
         return false;
      }

      if ((int)buffer.size() < delta_state->uncompressed_size) {
         return false;
      }

      for (int i = 0; i < delta_state->uncompressed_size; i++) {
         buffer[i] ^= _delta_buffer[i];
      }
   }

   return true;
}

void
Sync::LoadFrame(int frame)
{
   // find the frame in question
   if (frame == _framecount) {
      Log("Skipping NOP.\n");
      return;
   }

   // Move the head pointer back and load it up
   _savedstate.head = FindSavedFrameIndex(frame);
   SavedFrame *state = _savedstate.frames + _savedstate.head;

   Log("=== Loading frame info %d (size: %d  checksum: %08x).\n",
       state->frame, state->uncompressed_size, state->checksum);

   ASSERT(state->buf && state->cbuf);
   if (state->delta) {
      ASSERT(ReconstructFrame(frame, _decompress_buffer));
      _callbacks.load_game_state(&_decompress_buffer[0], state->uncompressed_size);
      UpdateLastState(&_decompress_buffer[0], state->uncompressed_size, state->frame);
   } else if (state->compressed) {
      ASSERT(state->uncompressed_size > 0);
      if (_decompress_buffer.size() < (size_t)state->uncompressed_size) {
         _decompress_buffer.resize(state->uncompressed_size);
      }
      byte *decompressed = &_decompress_buffer[0];
      int decoded = LZ4_decompress_safe((const char *)state->buf,
                                       (char *)decompressed,
                                       state->cbuf,
                                       state->uncompressed_size);
      ASSERT(decoded == state->uncompressed_size);
      _callbacks.load_game_state(decompressed, state->uncompressed_size);
      UpdateLastState(decompressed, state->uncompressed_size, state->frame);
   } else {
      _callbacks.load_game_state(state->buf, state->cbuf);
      UpdateLastState(state->buf, state->cbuf, state->frame);
   }

   // Reset framecount and the head of the state ring-buffer to point in
   // advance of the current frame (as if we had just finished executing it).
   _framecount = state->frame;
   _savedstate.head = (_savedstate.head + 1) % ARRAY_SIZE(_savedstate.frames);
}

void
Sync::SaveCurrentFrame()
{
   /*
    * See StateCompress for the real save feature implemented by FinalBurn.
    * Write everything into the head, then advance the head pointer.
    */
   SavedFrame *state = _savedstate.frames + _savedstate.head;
   if (state->buf) {
      FreeSavedFrameBuffer(*state);
   }
   state->frame = _framecount;
   _callbacks.save_game_state(&state->buf, &state->cbuf, &state->checksum, state->frame);
   state->uncompressed_size = state->cbuf;
   state->compressed = false;
   state->delta = false;

   bool can_delta = _last_state_valid
      && _last_state_size == state->uncompressed_size
      && _last_state_frame == (state->frame - 1);
   bool keyframe = (state->frame % GGPO_STATE_KEYFRAME_INTERVAL) == 0;
   bool use_delta = can_delta && !keyframe;

   if (use_delta) {
      if (_delta_buffer.size() < (size_t)state->uncompressed_size) {
         _delta_buffer.resize(state->uncompressed_size);
      }
      for (int i = 0; i < state->uncompressed_size; i++) {
         _delta_buffer[i] = state->buf[i] ^ _last_state[i];
      }
   }

   UpdateLastState(state->buf, state->uncompressed_size, state->frame);

   int max_compressed = LZ4_compressBound(state->uncompressed_size);
   char *compressed_buf = (char *)malloc(max_compressed);
   ASSERT(compressed_buf);

   const char *compress_input = use_delta
      ? (const char *)&_delta_buffer[0]
      : (const char *)state->buf;

   int compressed_size = LZ4_compress_fast(compress_input,
                                           compressed_buf,
                                           state->uncompressed_size,
                                           max_compressed,
                                           _lz4_accel);
   if (use_delta) {
      state->delta = true;
      if (compressed_size > 0 && compressed_size < state->uncompressed_size) {
         _callbacks.free_buffer(state->buf);
         state->buf = (byte *)compressed_buf;
         state->cbuf = compressed_size;
         state->compressed = true;
      } else {
         memcpy(compressed_buf, &_delta_buffer[0], state->uncompressed_size);
         _callbacks.free_buffer(state->buf);
         state->buf = (byte *)compressed_buf;
         state->cbuf = state->uncompressed_size;
         state->compressed = false;
      }
   } else {
      if (compressed_size > 0 && compressed_size < state->uncompressed_size) {
         _callbacks.free_buffer(state->buf);
         state->buf = (byte *)compressed_buf;
         state->cbuf = compressed_size;
         state->compressed = true;
      } else {
         free(compressed_buf);
      }
   }

   if (state->delta) {
      int ratio = 0;
      if (state->uncompressed_size > 0) {
         ratio = (int)(((unsigned long long)state->cbuf * 100ULL) /
                       (unsigned long long)state->uncompressed_size);
         if (ratio > 100) {
            ratio = 100;
         }
      }
      _delta_stats.delta_ratio_last = ratio;
      if (ratio > _delta_stats.delta_ratio_max) {
         _delta_stats.delta_ratio_max = ratio;
      }
      _delta_stats.delta_bytes_sum += (unsigned long long)state->cbuf;
      _delta_stats.delta_raw_bytes_sum += (unsigned long long)state->uncompressed_size;
      _delta_stats.delta_frames++;
   } else {
      _delta_stats.keyframes++;
   }

   Log("=== Saved frame info %d (size: %d  compressed: %d  checksum: %08x).\n",
       state->frame, state->uncompressed_size, state->cbuf, state->checksum);
   _savedstate.head = (_savedstate.head + 1) % ARRAY_SIZE(_savedstate.frames);
}

Sync::SavedFrame&
Sync::GetLastSavedFrame()
{
   int i = _savedstate.head - 1;
   if (i < 0) {
      i = ARRAY_SIZE(_savedstate.frames) - 1;
   }
   return _savedstate.frames[i];
}

void
Sync::FreeSavedFrameBuffer(SavedFrame &state)
{
   if (!state.buf) {
      return;
   }
   if (state.compressed || state.delta) {
      free(state.buf);
   } else {
      _callbacks.free_buffer(state.buf);
   }
   state.buf = NULL;
   state.cbuf = 0;
   state.uncompressed_size = 0;
   state.compressed = false;
   state.delta = false;
}


int
Sync::FindSavedFrameIndex(int frame)
{
   int i, count = ARRAY_SIZE(_savedstate.frames);
   for (i = 0; i < count; i++) {
      if (_savedstate.frames[i].frame == frame) {
         break;
      }
   }
   if (i == count) {
      ASSERT(FALSE);
   }
   return i;
}


bool
Sync::CreateQueues(Config &config)
{
   _input_queues.clear();
   _input_queues.resize(_config.num_players);

   for (int i = 0; i < _config.num_players; i++) {
      _input_queues[i].Init(i, _config.input_size);
   }
   return true;
}

bool
Sync::CheckSimulationConsistency(int *seekTo)
{
   int first_incorrect = GameInput::NullFrame;
   for (int i = 0; i < _config.num_players; i++) {
      int incorrect = _input_queues[i].GetFirstIncorrectFrame();
      Log("considering incorrect frame %d reported by queue %d.\n", incorrect, i);

      if (incorrect != GameInput::NullFrame && (first_incorrect == GameInput::NullFrame || incorrect < first_incorrect)) {
         first_incorrect = incorrect;
      }
   }

   if (first_incorrect == GameInput::NullFrame) {
      Log("prediction ok.  proceeding.\n");
      return true;
   }
   *seekTo = first_incorrect;
   return false;
}

void
Sync::SetFrameDelay(int queue, int delay)
{
   _input_queues[queue].SetFrameDelay(delay);
}


void
Sync::ResetPrediction(int frameNumber)
{
   for (int i = 0; i < _config.num_players; i++) {
      _input_queues[i].ResetPrediction(frameNumber);
   }
}


bool
Sync::GetEvent(Event &e)
{
   if (_event_queue.size()) {
      e = _event_queue.front();
      _event_queue.pop();
      return true;
   }
   return false;
}

void
Sync::GetStateStats(GGPOStateStats *stats)
{
   int avg_ratio = 0;

   if (!stats) {
      return;
   }

   memset(stats, 0, sizeof(*stats));
   stats->delta_frames = _delta_stats.delta_frames;
   stats->keyframes = _delta_stats.keyframes;
   stats->delta_ratio_last = _delta_stats.delta_ratio_last;
   stats->delta_ratio_max = _delta_stats.delta_ratio_max;

   if (_delta_stats.delta_raw_bytes_sum > 0) {
      avg_ratio = (int)((_delta_stats.delta_bytes_sum * 100ULL) /
                        _delta_stats.delta_raw_bytes_sum);
      if (avg_ratio > 100) {
         avg_ratio = 100;
      }
   }
   stats->delta_ratio_avg = avg_ratio;
}
