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

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#  define GGPO_SIMD_X86 1
#endif

#if defined(GGPO_SIMD_X86)
#  include <immintrin.h>
#  if !defined(_MSC_VER)
#    include <cpuid.h>
#  endif
#endif

namespace {

#if defined(__AVX2__)
#  define GGPO_SIMD_AVX2 1
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define GGPO_SIMD_SSE2 1
#endif

typedef void (*XorInPlaceFn)(byte *dst, const byte *src, size_t len);
typedef void (*XorBuffersFn)(byte *dst, const byte *lhs, const byte *rhs, size_t len);
typedef void (*MemcpyFn)(byte *dst, const byte *src, size_t len);

static void XorBufferInPlaceScalar(byte *dst, const byte *src, size_t len)
{
   for (size_t i = 0; i < len; ++i) {
      dst[i] ^= src[i];
   }
}

static void XorBuffersScalar(byte *dst, const byte *lhs, const byte *rhs, size_t len)
{
   for (size_t i = 0; i < len; ++i) {
      dst[i] = lhs[i] ^ rhs[i];
   }
}

static void FastMemcpyScalar(byte *dst, const byte *src, size_t len)
{
   if (dst == src || len == 0) {
      return;
   }
   memcpy(dst, src, len);
}

#if defined(GGPO_SIMD_X86)
static void Cpuid(int cpu_info[4], int leaf, int subleaf)
{
#if defined(_MSC_VER)
   __cpuidex(cpu_info, leaf, subleaf);
#elif defined(__GNUC__) || defined(__clang__)
   unsigned int a = 0, b = 0, c = 0, d = 0;
   __cpuid_count((unsigned int)leaf, (unsigned int)subleaf, a, b, c, d);
   cpu_info[0] = (int)a;
   cpu_info[1] = (int)b;
   cpu_info[2] = (int)c;
   cpu_info[3] = (int)d;
#else
   cpu_info[0] = cpu_info[1] = cpu_info[2] = cpu_info[3] = 0;
#endif
}

static unsigned long long Xgetbv(unsigned int index)
{
#if defined(_MSC_VER)
   return _xgetbv(index);
#elif defined(__GNUC__) || defined(__clang__)
   unsigned int eax = 0, edx = 0;
   __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
   return ((unsigned long long)edx << 32) | eax;
#else
   return 0;
#endif
}
#endif

#if defined(GGPO_SIMD_SSE2)
static bool CpuHasSse2()
{
   int info[4] = { 0, 0, 0, 0 };
   Cpuid(info, 1, 0);
   return (info[3] & (1 << 26)) != 0;
}

static void XorBufferInPlaceSse2(byte *dst, const byte *src, size_t len)
{
   size_t i = 0;
   size_t limit = len & ~(size_t)15;
   for (; i < limit; i += 16) {
      __m128i a = _mm_loadu_si128((const __m128i *)(dst + i));
      __m128i b = _mm_loadu_si128((const __m128i *)(src + i));
      _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(a, b));
   }
   for (; i < len; ++i) {
      dst[i] ^= src[i];
   }
}

static void XorBuffersSse2(byte *dst, const byte *lhs, const byte *rhs, size_t len)
{
   size_t i = 0;
   size_t limit = len & ~(size_t)15;
   for (; i < limit; i += 16) {
      __m128i a = _mm_loadu_si128((const __m128i *)(lhs + i));
      __m128i b = _mm_loadu_si128((const __m128i *)(rhs + i));
      _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(a, b));
   }
   for (; i < len; ++i) {
      dst[i] = lhs[i] ^ rhs[i];
   }
}

static void FastMemcpySse2(byte *dst, const byte *src, size_t len)
{
   if (dst == src || len == 0) {
      return;
   }
   if (len >= 32) {
      size_t i = 0;
      size_t limit = len & ~(size_t)15;
      for (; i < limit; i += 16) {
         __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
         _mm_storeu_si128((__m128i *)(dst + i), v);
      }
      for (; i < len; ++i) {
         dst[i] = src[i];
      }
      return;
   }
   memcpy(dst, src, len);
}
#endif

#if defined(GGPO_SIMD_AVX2)
static bool CpuHasAvx2()
{
   int info[4] = { 0, 0, 0, 0 };
   Cpuid(info, 1, 0);
   bool osxsave = (info[2] & (1 << 27)) != 0;
   bool avx = (info[2] & (1 << 28)) != 0;
   if (!osxsave || !avx) {
      return false;
   }
   unsigned long long xcr0 = Xgetbv(0);
   if ((xcr0 & 0x6) != 0x6) {
      return false;
   }
   Cpuid(info, 7, 0);
   return (info[1] & (1 << 5)) != 0;
}

static void XorBufferInPlaceAvx2(byte *dst, const byte *src, size_t len)
{
   size_t i = 0;
   size_t limit = len & ~(size_t)31;
   for (; i < limit; i += 32) {
      __m256i a = _mm256_loadu_si256((const __m256i *)(dst + i));
      __m256i b = _mm256_loadu_si256((const __m256i *)(src + i));
      _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(a, b));
   }
   for (; i < len; ++i) {
      dst[i] ^= src[i];
   }
   _mm256_zeroupper();
}

static void XorBuffersAvx2(byte *dst, const byte *lhs, const byte *rhs, size_t len)
{
   size_t i = 0;
   size_t limit = len & ~(size_t)31;
   for (; i < limit; i += 32) {
      __m256i a = _mm256_loadu_si256((const __m256i *)(lhs + i));
      __m256i b = _mm256_loadu_si256((const __m256i *)(rhs + i));
      _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(a, b));
   }
   for (; i < len; ++i) {
      dst[i] = lhs[i] ^ rhs[i];
   }
   _mm256_zeroupper();
}

static void FastMemcpyAvx2(byte *dst, const byte *src, size_t len)
{
   if (dst == src || len == 0) {
      return;
   }
   if (len >= 64) {
      size_t i = 0;
      size_t limit = len & ~(size_t)31;
      for (; i < limit; i += 32) {
         __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
         _mm256_storeu_si256((__m256i *)(dst + i), v);
      }
      for (; i < len; ++i) {
         dst[i] = src[i];
      }
      _mm256_zeroupper();
      return;
   }
   memcpy(dst, src, len);
}
#endif

static XorInPlaceFn g_xor_in_place = XorBufferInPlaceScalar;
static XorBuffersFn g_xor_buffers = XorBuffersScalar;
static MemcpyFn g_fast_memcpy = FastMemcpyScalar;
static std::once_flag g_simd_once;

static void InitSimdDispatch()
{
   std::call_once(g_simd_once, []() {
      g_xor_in_place = XorBufferInPlaceScalar;
      g_xor_buffers = XorBuffersScalar;
      g_fast_memcpy = FastMemcpyScalar;
#if defined(GGPO_SIMD_SSE2)
      if (CpuHasSse2()) {
         g_xor_in_place = XorBufferInPlaceSse2;
         g_xor_buffers = XorBuffersSse2;
         g_fast_memcpy = FastMemcpySse2;
      }
#endif
#if defined(GGPO_SIMD_AVX2)
      if (CpuHasAvx2()) {
         g_xor_in_place = XorBufferInPlaceAvx2;
         g_xor_buffers = XorBuffersAvx2;
         g_fast_memcpy = FastMemcpyAvx2;
      }
#endif
   });
}

static inline void XorBufferInPlace(byte *dst, const byte *src, size_t len)
{
   InitSimdDispatch();
   g_xor_in_place(dst, src, len);
}

static inline void XorBuffers(byte *dst, const byte *lhs, const byte *rhs, size_t len)
{
   InitSimdDispatch();
   g_xor_buffers(dst, lhs, rhs, len);
}

static inline void FastMemcpy(byte *dst, const byte *src, size_t len)
{
   InitSimdDispatch();
   g_fast_memcpy(dst, src, len);
}

} // namespace

Sync::Sync(UdpMsg::connect_status *connect_status) :
 _local_connect_status(connect_status)
{
   _framecount = 0;
   _last_confirmed_frame = -1;
   _max_prediction_frames = 0;
   _lz4_accel = 1;
   _async_compress = false;
   _compress_shutdown = false;
   _compress_jobs_max = 0;
   _compress_results_max = 0;
   _state_buffer_size_hint = 0;
   _last_state_size = 0;
   _last_state_frame = -1;
   _last_state_valid = false;
   memset(&_savedstate, 0, sizeof(_savedstate));
}

Sync::~Sync()
{
   StopCompressionThread();
   /*
    * Delete frames manually here rather than in a destructor of the SavedFrame
    * structure so we can efficently copy frames via weak references.
    */
   for (int i = 0; i < ARRAY_SIZE(_savedstate.frames); i++) {
      FreeSavedFrameBuffer(_savedstate.frames[i]);
   }
   ClearStateBufferPool();
   FreeScratchBuffer(_last_state);
   FreeScratchBuffer(_delta_buffer);
   FreeScratchBuffer(_decompress_buffer);
}

void
Sync::Init(Sync::Config &config)
{
   StopCompressionThread();
   ClearStateBufferPool();
   _config = config;
   _callbacks = config.callbacks;
   _framecount = 0;
   _rollingback = false;
   _last_state_valid = false;
   _last_state_size = 0;
   _last_state_frame = -1;
   ResetScratchBuffer(_last_state);
   ResetScratchBuffer(_delta_buffer);
   ResetScratchBuffer(_decompress_buffer);
   _delta_stats = DeltaStats();

   _max_prediction_frames = config.num_prediction_frames;
   _lz4_accel = config.lz4_accel;
   if (_lz4_accel <= 0) {
      _lz4_accel = Platform::GetConfigInt("ggpo.sync.lz4_accel");
      if (_lz4_accel <= 0) {
         _lz4_accel = 2;
      }
   }

   _async_compress = config.async_compress != 0;
   if (_async_compress) {
      StartCompressionThread();
   }

   CreateQueues(config);
}

void
Sync::StartCompressionThread()
{
   if (_compress_thread.joinable()) {
      return;
   }

   _compress_shutdown = false;
   _compress_jobs.clear();
   _compress_results.clear();
   _compress_jobs_max = 0;
   _compress_results_max = 0;

   _compress_thread = std::thread(&Sync::CompressionThreadMain, this);
}

void
Sync::StopCompressionThread()
{
   if (!_compress_thread.joinable()) {
      _async_compress = false;
      _compress_shutdown = false;
      _compress_jobs.clear();
      _compress_results.clear();
      _compress_jobs_max = 0;
      _compress_results_max = 0;
      return;
   }

   {
      std::unique_lock<std::mutex> lock(_compress_mutex);
      _compress_shutdown = true;
   }
   _compress_cv.notify_all();
   _compress_done_cv.notify_all();
   _compress_thread.join();

   {
      std::unique_lock<std::mutex> lock(_compress_mutex);
      for (size_t i = 0; i < _compress_jobs.size(); ++i) {
         if (_compress_jobs[i].state) {
            _compress_jobs[i].state->compress_pending = false;
         }
      }
      _compress_jobs.clear();

      for (size_t i = 0; i < _compress_results.size(); ++i) {
         if (_compress_results[i].compressed_buf) {
            free(_compress_results[i].compressed_buf);
         }
         if (_compress_results[i].state) {
            _compress_results[i].state->compress_pending = false;
         }
      }
      _compress_results.clear();
      _compress_shutdown = false;
      _compress_jobs_max = 0;
      _compress_results_max = 0;
   }

   _async_compress = false;
}

void
Sync::CompressionThreadMain()
{
   for (;;) {
      CompressJob job;
      {
         std::unique_lock<std::mutex> lock(_compress_mutex);
         _compress_cv.wait(lock, [this] {
            return _compress_shutdown || !_compress_jobs.empty();
         });
         if (_compress_shutdown && _compress_jobs.empty()) {
            return;
         }
         job = _compress_jobs.front();
         _compress_jobs.pop_front();
      }

      int max_compressed = LZ4_compressBound(job.input_size);
      char *compressed_buf = NULL;
      int compressed_size = 0;
      if (max_compressed > 0) {
         compressed_buf = (char *)malloc(max_compressed);
      }
      if (compressed_buf) {
         compressed_size = LZ4_compress_fast((const char *)job.input,
                                             compressed_buf,
                                             job.input_size,
                                             max_compressed,
                                             job.lz4_accel);
      }

      {
         std::unique_lock<std::mutex> lock(_compress_mutex);
         if (_compress_shutdown) {
            if (compressed_buf) {
               free(compressed_buf);
            }
            if (job.state) {
               job.state->compress_pending = false;
            }
         } else {
            CompressResult result;
            result.state = job.state;
            result.input = job.input;
            result.input_size = job.input_size;
            result.frame = job.frame;
            result.compressed_buf = compressed_buf;
            result.compressed_size = compressed_size;
            _compress_results.push_back(result);
            if ((int)_compress_results.size() > _compress_results_max) {
               _compress_results_max = (int)_compress_results.size();
            }
         }
      }
      _compress_done_cv.notify_all();
   }
}

bool
Sync::QueueCompression(SavedFrame *state, const byte *input, int input_size)
{
   if (!_async_compress || !state || !input || input_size <= 0) {
      return false;
   }
   if (!_compress_thread.joinable()) {
      return false;
   }

   std::unique_lock<std::mutex> lock(_compress_mutex);
   if (_compress_shutdown || state->compress_pending) {
      return false;
   }
   size_t max_queue = ARRAY_SIZE(_savedstate.frames);
   if (_compress_jobs.size() + _compress_results.size() >= max_queue) {
      return false;
   }
   CompressJob job;
   job.state = state;
   job.input = input;
   job.input_size = input_size;
   job.frame = state->frame;
   job.lz4_accel = _lz4_accel;
   _compress_jobs.push_back(job);
   if ((int)_compress_jobs.size() > _compress_jobs_max) {
      _compress_jobs_max = (int)_compress_jobs.size();
   }
   state->compress_pending = true;
   lock.unlock();

   _compress_cv.notify_one();
   return true;
}

void
Sync::ProcessCompressionResults()
{
   if (!_async_compress) {
      return;
   }

   for (;;) {
      CompressResult result;
      {
         std::unique_lock<std::mutex> lock(_compress_mutex);
         if (_compress_results.empty()) {
            return;
         }
         result = _compress_results.front();
         _compress_results.pop_front();
      }
      ApplyCompressionResult(result);
   }
}

void
Sync::ApplyCompressionResult(const CompressResult &result)
{
   if (!result.state) {
      if (result.compressed_buf) {
         free(result.compressed_buf);
      }
      return;
   }

   SavedFrame *state = result.state;
   if (state->compress_pending) {
      state->compress_pending = false;
   }

   if (!result.compressed_buf || result.compressed_size <= 0) {
      if (result.compressed_buf) {
         free(result.compressed_buf);
      }
      return;
   }

   if (state->frame != result.frame || state->buf != result.input || state->compressed) {
      free(result.compressed_buf);
      return;
   }

   if (result.compressed_size >= state->uncompressed_size) {
      free(result.compressed_buf);
      return;
   }

   byte *old_buf = state->buf;
   if (state->compressed || state->delta) {
      free(old_buf);
   } else {
      RecycleStateBuffer(old_buf, state->buf_capacity);
   }
   state->buf = (byte *)result.compressed_buf;
   state->cbuf = result.compressed_size;
   state->buf_capacity = result.compressed_size;
   state->compressed = true;
}

void
Sync::WaitForCompression(SavedFrame &state)
{
   if (!_async_compress || !state.compress_pending) {
      return;
   }

   for (;;) {
      ProcessCompressionResults();
      if (!state.compress_pending) {
         break;
      }
      std::unique_lock<std::mutex> lock(_compress_mutex);
      _compress_done_cv.wait(lock, [this] {
         return !_compress_results.empty() || _compress_shutdown;
      });
      if (_compress_shutdown) {
         state.compress_pending = false;
         break;
      }
   }
}

void
Sync::CompressSync(SavedFrame &state, const byte *input, int input_size)
{
   int max_compressed = LZ4_compressBound(input_size);
   char *compressed_buf = (char *)malloc(max_compressed);
   if (!compressed_buf) {
      return;
   }

   int compressed_size = LZ4_compress_fast((const char *)input,
                                           compressed_buf,
                                           input_size,
                                           max_compressed,
                                           _lz4_accel);
   if (compressed_size > 0 && compressed_size < input_size) {
      byte *old_buf = state.buf;
      if (state.compressed || state.delta) {
         free(old_buf);
      } else {
         RecycleStateBuffer(old_buf, state.buf_capacity);
      }
      state.buf = (byte *)compressed_buf;
      state.cbuf = compressed_size;
      state.buf_capacity = compressed_size;
      state.compressed = true;
   } else {
      free(compressed_buf);
   }
}

byte *
Sync::AcquireStateBuffer(int *capacity)
{
   if (capacity) {
      *capacity = 0;
   }
   if (_state_buffer_size_hint <= 0 || _state_buffer_pool.empty()) {
      return NULL;
   }

   int best_index = -1;
   int best_capacity = 0;
   for (size_t i = 0; i < _state_buffer_pool.size(); ++i) {
      const StateBuffer &entry = _state_buffer_pool[i];
      if (entry.capacity >= _state_buffer_size_hint) {
         if (best_index < 0 || entry.capacity < best_capacity) {
            best_index = (int)i;
            best_capacity = entry.capacity;
         }
      }
   }

   if (best_index < 0) {
      return NULL;
   }

   StateBuffer entry = _state_buffer_pool[best_index];
   _state_buffer_pool.erase(_state_buffer_pool.begin() + best_index);
   if (capacity) {
      *capacity = entry.capacity;
   }
   return entry.buffer;
}

void
Sync::RecycleStateBuffer(byte *buffer, int capacity)
{
   if (!buffer) {
      return;
   }

   if (capacity <= 0 || !_callbacks.free_buffer) {
      if (_callbacks.free_buffer) {
         _callbacks.free_buffer(buffer);
      }
      return;
   }

   int max_pool = ARRAY_SIZE(_savedstate.frames);
   if ((int)_state_buffer_pool.size() >= max_pool) {
      _callbacks.free_buffer(buffer);
      return;
   }

   StateBuffer entry;
   entry.buffer = buffer;
   entry.capacity = capacity;
   _state_buffer_pool.push_back(entry);
}

void
Sync::ClearStateBufferPool()
{
   if (!_callbacks.free_buffer) {
      _state_buffer_pool.clear();
      _state_buffer_size_hint = 0;
      return;
   }
   for (size_t i = 0; i < _state_buffer_pool.size(); ++i) {
      if (_state_buffer_pool[i].buffer) {
         _callbacks.free_buffer(_state_buffer_pool[i].buffer);
      }
   }
   _state_buffer_pool.clear();
   _state_buffer_size_hint = 0;
}

void
Sync::ResetScratchBuffer(ScratchBuffer &buffer)
{
   buffer.size = 0;
}

void
Sync::FreeScratchBuffer(ScratchBuffer &buffer)
{
   if (buffer.data) {
      free(buffer.data);
   }
   buffer.data = NULL;
   buffer.size = 0;
   buffer.capacity = 0;
}

void
Sync::EnsureScratchBufferSize(ScratchBuffer &buffer, int size)
{
   if (size <= 0) {
      buffer.size = 0;
      return;
   }
   if (buffer.capacity < size) {
      byte *new_buf = (byte *)realloc(buffer.data, size);
      ASSERT(new_buf);
      buffer.data = new_buf;
      buffer.capacity = size;
   }
   buffer.size = size;
}

bool
Sync::DecodeSavedFrameRaw(const SavedFrame &state, byte *buffer, int buffer_size)
{
   if (!state.buf || state.uncompressed_size <= 0 || !buffer || buffer_size < state.uncompressed_size) {
      return false;
   }

   if (state.compressed) {
      int decoded = LZ4_decompress_safe((const char *)state.buf,
                                        (char *)buffer,
                                        state.cbuf,
                                        state.uncompressed_size);
      return decoded == state.uncompressed_size;
   }

   FastMemcpy(buffer, state.buf, (size_t)state.uncompressed_size);
   return true;
}

bool
Sync::DecodeSavedFrameInternal(const SavedFrame &state, ScratchBuffer &buffer)
{
   if (!state.buf || state.uncompressed_size <= 0) {
      return false;
   }
   EnsureScratchBufferSize(buffer, state.uncompressed_size);
   return DecodeSavedFrameRaw(state, buffer.data, buffer.size);
}

bool
Sync::ReconstructFrameInternal(int frame, ScratchBuffer &buffer)
{
   int base_frame = frame;
   bool found_base = false;
   int state_index = FindSavedFrameIndex(frame);
   SavedFrame *state = NULL;

   if (state_index < 0) {
      return false;
   }

   state = _savedstate.frames + state_index;

   if (!state->delta) {
      return DecodeSavedFrameInternal(*state, buffer);
   }

   while (base_frame >= 0) {
      int base_index = FindSavedFrameIndex(base_frame);
      SavedFrame *base_state = NULL;

      if (base_index < 0) {
         return false;
      }

      base_state = _savedstate.frames + base_index;
      if (!base_state->delta) {
         if (!DecodeSavedFrameInternal(*base_state, buffer)) {
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
      int delta_index = FindSavedFrameIndex(f);
      SavedFrame *delta_state = NULL;

      if (delta_index < 0) {
         return false;
      }

      delta_state = _savedstate.frames + delta_index;
      if (!delta_state->delta) {
         if (!DecodeSavedFrameInternal(*delta_state, buffer)) {
            return false;
         }
         continue;
      }

      EnsureScratchBufferSize(_delta_buffer, delta_state->uncompressed_size);

      if (!DecodeSavedFrameInternal(*delta_state, _delta_buffer)) {
         return false;
      }

      if (buffer.size < delta_state->uncompressed_size) {
         return false;
      }

      XorBufferInPlace(buffer.data, _delta_buffer.data, (size_t)delta_state->uncompressed_size);
   }

   return true;
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
   if (!LoadFrame(seek_to) || _framecount != seek_to) {
      Log("Failed to load frame %d for rollback (have %d). Clearing prediction errors.\n",
          seek_to, _framecount);
      ResetPrediction(seek_to);
      _rollingback = false;
      return;
   }

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
      ResetScratchBuffer(_last_state);
      return;
   }

   EnsureScratchBufferSize(_last_state, size);
   FastMemcpy(_last_state.data, state, (size_t)size);
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

   return DecodeSavedFrameRaw(state, &buffer[0], (int)buffer.size());
}

bool
Sync::ReconstructFrame(int frame, std::vector<byte> &buffer)
{
   int base_frame = frame;
   bool found_base = false;
   int state_index = FindSavedFrameIndex(frame);
   SavedFrame *state = NULL;

   if (state_index < 0) {
      return false;
   }

   state = _savedstate.frames + state_index;

   if (!state->delta) {
      return DecodeSavedFrame(*state, buffer);
   }

   while (base_frame >= 0) {
      int base_index = FindSavedFrameIndex(base_frame);
      SavedFrame *base_state = NULL;

      if (base_index < 0) {
         return false;
      }

      base_state = _savedstate.frames + base_index;
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
      int delta_index = FindSavedFrameIndex(f);
      SavedFrame *delta_state = NULL;

      if (delta_index < 0) {
         return false;
      }

      delta_state = _savedstate.frames + delta_index;
      if (!delta_state->delta) {
         if (!DecodeSavedFrame(*delta_state, buffer)) {
            return false;
         }
         continue;
      }

      EnsureScratchBufferSize(_delta_buffer, delta_state->uncompressed_size);

      if (!DecodeSavedFrameInternal(*delta_state, _delta_buffer)) {
         return false;
      }

      if ((int)buffer.size() < delta_state->uncompressed_size) {
         return false;
      }

      XorBufferInPlace(&buffer[0], _delta_buffer.data, (size_t)delta_state->uncompressed_size);
   }

   return true;
}

bool
Sync::LoadFrame(int frame)
{
   // find the frame in question
   if (frame == _framecount) {
      Log("Skipping NOP.\n");
      return true;
   }

   // Move the head pointer back and load it up
   _savedstate.head = FindSavedFrameIndex(frame);
   if (_savedstate.head < 0) {
      return false;
   }
   SavedFrame *state = _savedstate.frames + _savedstate.head;

   Log("=== Loading frame info %d (size: %d  checksum: %08x).\n",
       state->frame, state->uncompressed_size, state->checksum);

   if (!state->buf || !state->cbuf) {
      Log("Cannot load frame %d: missing state buffer.\n", frame);
      return false;
   }
   if (state->delta) {
      if (!ReconstructFrameInternal(frame, _decompress_buffer)) {
         Log("Failed to reconstruct frame %d.\n", frame);
         return false;
      }
      _callbacks.load_game_state(_decompress_buffer.data, state->uncompressed_size);
      UpdateLastState(_decompress_buffer.data, state->uncompressed_size, state->frame);
   } else if (state->compressed) {
      if (state->uncompressed_size <= 0) {
         Log("Invalid compressed size for frame %d.\n", frame);
         return false;
      }
      EnsureScratchBufferSize(_decompress_buffer, state->uncompressed_size);
      byte *decompressed = _decompress_buffer.data;
      int decoded = LZ4_decompress_safe((const char *)state->buf,
                                       (char *)decompressed,
                                       state->cbuf,
                                       state->uncompressed_size);
      if (decoded != state->uncompressed_size) {
         Log("Failed to decompress frame %d.\n", frame);
         return false;
      }
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
   return true;
}

void
Sync::SaveCurrentFrame()
{
   /*
    * See StateCompress for the real save feature implemented by FinalBurn.
    * Write everything into the head, then advance the head pointer.
    */
   ProcessCompressionResults();

   SavedFrame *state = _savedstate.frames + _savedstate.head;
   if (state->buf) {
      FreeSavedFrameBuffer(*state);
   }
   state->frame = _framecount;
   int reuse_capacity = 0;
   byte *reuse_buffer = AcquireStateBuffer(&reuse_capacity);
   state->buf = reuse_buffer;
   state->cbuf = reuse_capacity;
   state->buf_capacity = reuse_capacity;
   _callbacks.save_game_state(&state->buf, &state->cbuf, &state->checksum, state->frame);
   if (reuse_buffer && state->buf != reuse_buffer) {
      RecycleStateBuffer(reuse_buffer, reuse_capacity);
   }
   if (state->buf == reuse_buffer) {
      if (state->cbuf > reuse_capacity) {
         Log("save_game_state used %d bytes but only %d were available.\n", state->cbuf, reuse_capacity);
      }
      state->buf_capacity = reuse_capacity;
   } else {
      state->buf_capacity = state->cbuf;
   }
   state->uncompressed_size = state->cbuf;
   state->compressed = false;
   state->delta = false;
   state->compress_pending = false;
   if (state->uncompressed_size > _state_buffer_size_hint) {
      _state_buffer_size_hint = state->uncompressed_size;
   }

   bool can_delta = _last_state_valid
      && _last_state_size == state->uncompressed_size
      && _last_state_frame == (state->frame - 1);
   bool keyframe = (state->frame % GGPO_STATE_KEYFRAME_INTERVAL) == 0;
   bool use_delta = can_delta && !keyframe;

   byte *delta_buf = NULL;
   if (use_delta) {
      delta_buf = (byte *)malloc(state->uncompressed_size);
      ASSERT(delta_buf);
      XorBuffers(delta_buf, state->buf, _last_state.data, (size_t)state->uncompressed_size);
   }

   UpdateLastState(state->buf, state->uncompressed_size, state->frame);

   if (use_delta) {
      state->delta = true;
      RecycleStateBuffer(state->buf, state->buf_capacity);
      state->buf = delta_buf;
      state->cbuf = state->uncompressed_size;
      state->buf_capacity = state->uncompressed_size;
      state->compressed = false;
   }

   const byte *compress_input = state->buf;
   if (!QueueCompression(state, compress_input, state->uncompressed_size)) {
      CompressSync(*state, compress_input, state->uncompressed_size);
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
   if (state.compress_pending) {
      WaitForCompression(state);
   }
   if (state.compressed || state.delta) {
      free(state.buf);
   } else {
      RecycleStateBuffer(state.buf, state.buf_capacity);
   }
   state.buf = NULL;
   state.cbuf = 0;
   state.uncompressed_size = 0;
   state.buf_capacity = 0;
   state.compressed = false;
   state.delta = false;
   state.compress_pending = false;
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
      Log("Saved frame %d not found in buffer.\n", frame);
      return -1;
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

   {
      std::unique_lock<std::mutex> lock(_compress_mutex);
      stats->compress_job_queue_len = (int)_compress_jobs.size();
      stats->compress_result_queue_len = (int)_compress_results.size();
      stats->compress_job_queue_max = _compress_jobs_max;
      stats->compress_result_queue_max = _compress_results_max;
   }

   int pending = 0;
   for (int i = 0; i < ARRAY_SIZE(_savedstate.frames); ++i) {
      if (_savedstate.frames[i].compress_pending) {
         pending++;
      }
   }
   stats->compress_pending_count = pending;
}
