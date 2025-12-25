/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2017 - Alfred Agrell
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <retro_inline.h>
#include <compat/strl.h>
#include <lz4.h>

#include "state_manager.h"
#include "msg_hash.h"
#include "core.h"
#include "core_info.h"
#include "retroarch.h"
#include "verbosity.h"
#include "content.h"
#include "audio/audio_driver.h"

#ifdef HAVE_NETWORKING
#include "network/netplay/netplay.h"
#endif

/* This makes Valgrind throw errors if a core overflows its savestate size. */
/* Keep it off unless you're chasing a core bug, it slows things down. */
#define STRICT_BUF_SIZE 0

#define STATE_DELTA_FLAG_RAW ((size_t)1)
#define STATE_DELTA_HEADER_SIZE (sizeof(size_t) * 2)

/* Returns the maximum compressed size of a savestate. */
static size_t state_manager_raw_maxsize(size_t uncomp)
{
   size_t bound = uncomp;

   if (uncomp <= (size_t)LZ4_MAX_INPUT_SIZE)
      bound = (size_t)LZ4_compressBound((int)uncomp);

   return STATE_DELTA_HEADER_SIZE + bound;
}

/*
 * See state_manager_raw_compress for information about this.
 * When you're done with it, send it to free().
 */
static void *state_manager_raw_alloc(size_t len, uint16_t uniq)
{
   size_t  _len  = (len + sizeof(uint16_t) - 1) & -sizeof(uint16_t);
   uint16_t *ret = (uint16_t*)calloc(_len + sizeof(uint16_t) * 4 + 16, 1);

   if (!ret)
      return NULL;

   /* Force in a different byte at the end, so we don't need to check
    * bounds in the innermost loop (it's expensive).
    *
    * There is also a large amount of data that's the same, to stop
    * the other scan.
    *
    * There is also some padding at the end. This is so we don't
    * read outside the buffer end if we're reading in large blocks;
    *
    * It doesn't make any difference to us, but sacrificing 16 bytes to get
    * Valgrind happy is worth it. */
   ret[_len / sizeof(uint16_t) + 3] = uniq;

   return ret;
}

static void state_manager_xor_delta(uint8_t *dst,
      const uint8_t *a, const uint8_t *b, size_t len)
{
   size_t i;

   for (i = 0; i < len; i++)
      dst[i] = a[i] ^ b[i];
}

/*
 * Takes two savestates and creates a patch that turns 'dst' into 'src'.
 * Both 'src' and 'dst' must be returned from state_manager_raw_alloc(),
 * with the same 'len'.
 *
 * 'patch' must be size 'state_manager_raw_maxsize(len)' or more.
 * Returns the number of bytes actually written to 'patch'.
 */
static size_t state_manager_raw_compress(const void *src,
      const void *dst, size_t len, void *patch, uint8_t *scratch)
{
   size_t *header          = (size_t*)patch;
   uint8_t *payload        = (uint8_t*)patch + STATE_DELTA_HEADER_SIZE;
   size_t payload_size     = 0;
   size_t flags            = 0;
   bool   use_compressed   = false;

   state_manager_xor_delta(scratch,
         (const uint8_t*)src, (const uint8_t*)dst, len);

   if (len <= (size_t)LZ4_MAX_INPUT_SIZE)
   {
      int max_out     = LZ4_compressBound((int)len);
      int compressed  = LZ4_compress_fast((const char*)scratch,
            (char*)payload, (int)len, max_out, 1);

      if (compressed > 0 && (size_t)compressed < len)
      {
         payload_size   = (size_t)compressed;
         use_compressed = true;
      }
   }

   if (!use_compressed)
   {
      memcpy(payload, scratch, len);
      payload_size = len;
      flags |= STATE_DELTA_FLAG_RAW;
   }

   header[0] = payload_size;
   header[1] = flags;

   return STATE_DELTA_HEADER_SIZE + payload_size;
}

/*
 * Takes 'patch' from a previous call to 'state_manager_raw_compress'
 * and applies it to 'data' ('dst' from that call),
 * yielding 'src' in that call.
 */
static bool state_manager_raw_decompress(const void *patch, void *data,
      size_t len, uint8_t *scratch)
{
   const size_t *header    = (const size_t*)patch;
   const uint8_t *payload  = (const uint8_t*)patch + STATE_DELTA_HEADER_SIZE;
   size_t payload_size     = header[0];
   size_t flags            = header[1];
   size_t i;

   if (flags & STATE_DELTA_FLAG_RAW)
   {
      if (payload_size != len)
         return false;
      memcpy(scratch, payload, len);
   }
   else
   {
      int decoded;

      if (len > (size_t)INT_MAX)
         return false;
      if (payload_size > (size_t)INT_MAX)
         return false;

      decoded = LZ4_decompress_safe((const char*)payload,
            (char*)scratch, (int)payload_size, (int)len);
      if (decoded != (int)len)
         return false;
   }

   for (i = 0; i < len; i++)
      ((uint8_t*)data)[i] ^= scratch[i];

   return true;
}

/* The start offsets point to 'nextstart' of any given compressed frame.
 * Each uint16 is stored native endian; anything that claims any other
 * endianness refers to the endianness of this specific item.
 * The uint32 is stored little endian.
 *
 * Each size value is stored native endian if alignment is not enforced;
 * if it is, they're little endian.
 *
 * The start of the buffer contains a size pointing to the end of the
 * buffer; the end points to its start.
 *
 * Wrapping is handled by returning to the start of the buffer if the
 * compressed data could potentially hit the edge;
 *
 * if the compressed data could potentially overwrite the tail pointer,
 * the tail retreats until it can no longer collide.
 *
 * This means that on average, ~2 * maxcompsize is
 * unused at any given moment. */

/* These are called very few constant times per frame,
 * keep it as simple as possible. */
static INLINE void write_size_t(void *ptr, size_t val)
{
   memcpy(ptr, &val, sizeof(val));
}

static INLINE size_t read_size_t(const void *ptr)
{
   size_t ret;

   memcpy(&ret, ptr, sizeof(ret));
   return ret;
}

static void state_manager_free(state_manager_t *state)
{
   if (!state)
      return;

   if (state->data)
      free(state->data);
   if (state->thisblock)
      free(state->thisblock);
   if (state->nextblock)
      free(state->nextblock);
   if (state->delta)
      free(state->delta);
#if STRICT_BUF_SIZE
   if (state->debugblock)
      free(state->debugblock);
   state->debugblock = NULL;
#endif
   state->data       = NULL;
   state->thisblock  = NULL;
   state->nextblock  = NULL;
   state->delta      = NULL;
}

static state_manager_t *state_manager_new(
      size_t state_size, size_t buffer_size)
{
   size_t max_comp_size, block_size;
   uint8_t *next_block    = NULL;
   uint8_t *this_block    = NULL;
   uint8_t *delta_block   = NULL;
   uint8_t *state_data    = NULL;
   state_manager_t *state = (state_manager_t*)calloc(1, sizeof(*state));

   if (!state)
      return NULL;

   block_size         = (state_size + sizeof(uint16_t) - 1) & -sizeof(uint16_t);
   /* the compressed data is surrounded by pointers to the other side */
   max_comp_size      = state_manager_raw_maxsize(block_size) + sizeof(size_t) * 2;
   state_data         = (uint8_t*)malloc(buffer_size);

   if (!state_data)
      goto error;

   this_block         = (uint8_t*)state_manager_raw_alloc(state_size, 0);
   next_block         = (uint8_t*)state_manager_raw_alloc(state_size, 1);
   delta_block        = (uint8_t*)malloc(block_size);

   if (!this_block || !next_block || !delta_block)
      goto error;

   state->blocksize   = block_size;
   state->maxcompsize = max_comp_size;
   state->data        = state_data;
   state->thisblock   = this_block;
   state->nextblock   = next_block;
   state->delta       = delta_block;
   state->capacity    = buffer_size;

   state->head        = state->data + sizeof(size_t);
   state->tail        = state->data + sizeof(size_t);

#if STRICT_BUF_SIZE
   state->debugsize   = state_size;
   state->debugblock  = (uint8_t*)malloc(state_size);
#endif

   return state;

error:
   if (state_data)
      free(state_data);
   state_manager_free(state);
   free(state);

   return NULL;
}

static bool state_manager_pop(state_manager_t *state, const void **data)
{
   size_t start;
   uint8_t *out                 = NULL;
   const uint8_t *compressed    = NULL;

   *data                        = NULL;

   if (state->thisblock_valid)
   {
      state->thisblock_valid    = false;
      state->entries--;
      *data                     = state->thisblock;
      return true;
   }

   *data                        = state->thisblock;
   if (state->head == state->tail)
      return false;

   start                        = read_size_t(state->head - sizeof(size_t));
   state->head                  = state->data + start;
   compressed                   = state->data + start + sizeof(size_t);
   out                          = state->thisblock;

   if (!state_manager_raw_decompress(compressed, out,
            state->blocksize, state->delta))
   {
      RARCH_ERR("[Rewind] Failed to decode state delta.\n");
      return false;
   }

   state->entries--;
   return true;
}

static void state_manager_push_where(state_manager_t *state, void **data)
{
   /* We need to ensure we have an uncompressed copy of the last
    * pushed state, or we could end up applying a 'patch' to wrong
    * savestate, and that'd blow up rather quickly. */

   if (!state->thisblock_valid)
   {
      const void *ignored;
      if (state_manager_pop(state, &ignored))
      {
         state->thisblock_valid = true;
         state->entries++;
      }
   }

   *data = state->nextblock;
#if STRICT_BUF_SIZE
   *data = state->debugblock;
#endif
}

static void state_manager_push_do(state_manager_t *state)
{
   uint8_t *swap = NULL;

#if STRICT_BUF_SIZE
   memcpy(state->nextblock, state->debugblock, state->debugsize);
#endif

   if (state->thisblock_valid)
   {
      uint8_t *compressed;
      const uint8_t *oldb, *newb;
      size_t headpos, tailpos, remaining;
      if (state->capacity < sizeof(size_t) + state->maxcompsize)
      {
         RARCH_ERR("[Rewind] %s.\n",
               msg_hash_to_str(MSG_REWIND_BUFFER_CAPACITY_INSUFFICIENT));
         return;
      }

recheckcapacity:;
      headpos   = state->head - state->data;
      tailpos   = state->tail - state->data;
      remaining = (tailpos + state->capacity -
            sizeof(size_t) - headpos - 1) % state->capacity + 1;

      if (remaining <= state->maxcompsize)
      {
         state->tail = state->data + read_size_t(state->tail);
         state->entries--;
         goto recheckcapacity;
      }

      oldb              = state->thisblock;
      newb              = state->nextblock;
      compressed        = state->head + sizeof(size_t);

      compressed       += state_manager_raw_compress(oldb, newb,
            state->blocksize, compressed, state->delta);

      if (compressed - state->data + state->maxcompsize > state->capacity)
      {
         compressed     = state->data;
         if (state->tail == state->data + sizeof(size_t))
            state->tail = state->data + read_size_t(state->tail);
      }
      write_size_t(compressed, state->head-state->data);
      compressed       += sizeof(size_t);
      write_size_t(state->head, compressed-state->data);
      state->head       = compressed;
   }
   else
      state->thisblock_valid = true;

   swap                      = state->thisblock;
   state->thisblock          = state->nextblock;
   state->nextblock          = swap;

   state->entries++;
}

void state_manager_event_init(
      struct state_manager_rewind_state *rewind_st,
      unsigned rewind_buffer_size)
{
   core_info_t *core_info = NULL;
   void *state            = NULL;

   if (  !rewind_st
       || (rewind_st->flags & STATE_MGR_REWIND_ST_FLAG_INIT_ATTEMPTED)
       || rewind_st->state)
      return;

   rewind_st->size               = 0;
   rewind_st->flags             &= ~(
                                   STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED
                                 | STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_CHECKED
                                 | STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED
                                    );

   /* We cannot initialise the rewind buffer
    * unless the core info struct for the current
    * core has been initialised (i.e. without this,
    * the savestate support level for the current
    * core is unknown) */
   if (!core_info_get_current_core(&core_info) || !core_info)
      return;

   rewind_st->flags |= STATE_MGR_REWIND_ST_FLAG_INIT_ATTEMPTED;

   if (!core_info_current_supports_rewind())
   {
      RARCH_ERR("[Rewind] %s.\n",
            msg_hash_to_str(MSG_REWIND_UNSUPPORTED));
      return;
   }

   if (audio_driver_has_callback())
   {
      RARCH_ERR("[Rewind] %s.\n",
            msg_hash_to_str(MSG_REWIND_INIT_FAILED_THREADED_AUDIO));
      return;
   }

   rewind_st->size = content_get_serialized_size_rewind();

   if (!rewind_st->size)
   {
      RARCH_ERR("[Rewind] %s.\n",
            msg_hash_to_str(MSG_REWIND_INIT_FAILED));
      return;
   }

   RARCH_LOG("[Rewind] %s: %u MB\n",
         msg_hash_to_str(MSG_REWIND_INIT),
         (unsigned)(rewind_buffer_size / 1000000));

   rewind_st->state = state_manager_new(rewind_st->size,
         rewind_buffer_size);

   if (!rewind_st->state)
      RARCH_WARN("[Rewind] %s.\n",
            msg_hash_to_str(MSG_REWIND_INIT_FAILED));

   state_manager_push_where(rewind_st->state, &state);

   content_serialize_state_rewind(state, rewind_st->size);

   state_manager_push_do(rewind_st->state);
}

void state_manager_event_deinit(
      struct state_manager_rewind_state *rewind_st,
      struct retro_core_t *current_core)
{
   bool restore_callbacks = false;

   if (!rewind_st)
      return;

   restore_callbacks =
            (rewind_st->flags & STATE_MGR_REWIND_ST_FLAG_INIT_ATTEMPTED)
         && (rewind_st->state)
         && (current_core);

   if (rewind_st->state)
   {
      state_manager_free(rewind_st->state);
      free(rewind_st->state);
   }

   rewind_st->state  = NULL;
   rewind_st->size   = 0;
   rewind_st->flags &= ~(
                          STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED
                        | STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_CHECKED
                        | STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED
                        | STATE_MGR_REWIND_ST_FLAG_INIT_ATTEMPTED
                        );

   /* Restore regular (non-rewind) core audio
    * callbacks if required */
   if (restore_callbacks)
   {
      if (current_core->retro_set_audio_sample)
         current_core->retro_set_audio_sample(audio_driver_sample);

      if (current_core->retro_set_audio_sample_batch)
         current_core->retro_set_audio_sample_batch(audio_driver_sample_batch);
   }
}

/**
 * check_rewind:
 * @pressed              : was rewind key pressed or held?
 *
 * Checks if rewind toggle/hold was being pressed and/or held.
 **/
bool state_manager_check_rewind(
      struct state_manager_rewind_state *rewind_st,
      struct retro_core_t *current_core,
      bool pressed,
      unsigned rewind_granularity, bool is_paused,
      char *s, size_t len, unsigned *time)
{
   bool ret          = false;
#ifdef HAVE_NETWORKING
   bool was_reversed = false;
#endif

   if (    !rewind_st
       || (!(rewind_st->flags & STATE_MGR_REWIND_ST_FLAG_INIT_ATTEMPTED)))
      return false;

   if (!(rewind_st->flags & STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_CHECKED))
   {
      rewind_st->flags |= STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_CHECKED;
      return false;
   }

   if (!rewind_st->state)
   {
      if ((pressed
          && (!(rewind_st->flags
                & STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED)))
          && !core_info_current_supports_rewind())
      {
         const char *_msg = msg_hash_to_str(MSG_REWIND_UNSUPPORTED);
         runloop_msg_queue_push(_msg, strlen(_msg), 1, 100, false, NULL,
               MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      }

      if (pressed)
         rewind_st->flags |=  STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED;
      else
         rewind_st->flags &= ~STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED;
      return false;
   }

   if (rewind_st->flags & STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED)
   {
#ifdef HAVE_NETWORKING
      was_reversed = true;
#endif
      audio_driver_frame_is_reverse();
      rewind_st->flags &= ~STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED;
   }

   if (pressed)
   {
      const void *buf    = NULL;

      if (state_manager_pop(rewind_st->state, &buf))
      {
#ifdef HAVE_NETWORKING
         /* Make sure netplay isn't confused */
         if (!was_reversed
               && !netplay_driver_ctl(RARCH_NETPLAY_CTL_DESYNC_PUSH, NULL))
            return false;
#endif

         rewind_st->flags |= STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED;

         audio_driver_setup_rewind();

         strlcpy(s, msg_hash_to_str(MSG_REWINDING), len);

         *time                  = is_paused ? 1 : 30;
         ret                    = true;

         content_deserialize_state(buf, rewind_st->size);

#ifdef HAVE_BSV_MOVIE
         bsv_movie_frame_rewind();
#endif
      }
      else
      {
#ifdef HAVE_BSV_MOVIE
         input_driver_state_t *input_st = input_state_get_ptr();
         /* Don't end reversing during playback or recording */
         if(BSV_MOVIE_IS_PLAYBACK_ON() || BSV_MOVIE_IS_RECORDING())
         {
            rewind_st->flags |= STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED;
            bsv_movie_frame_rewind();
         }
         else
#endif
            content_deserialize_state(buf, rewind_st->size);

#ifdef HAVE_NETWORKING
         /* Tell netplay we're done */
         if (was_reversed)
            netplay_driver_ctl(RARCH_NETPLAY_CTL_DESYNC_POP, NULL);
#endif

         strlcpy(s,
               msg_hash_to_str(MSG_REWIND_REACHED_END),
               len);

         *time = 30;
         ret   = true;
      }
   }
   else
   {
      static unsigned cnt      = 0;

#ifdef HAVE_NETWORKING
      /* Tell netplay we're done */
      if (was_reversed)
         netplay_driver_ctl(RARCH_NETPLAY_CTL_DESYNC_POP, NULL);
#endif

      cnt = (cnt + 1) % (rewind_granularity ?
            rewind_granularity : 1); /* Avoid possible SIGFPE. */

      if (     !is_paused
            && ((cnt == 0) || retroarch_ctl(RARCH_CTL_BSV_MOVIE_IS_INITED, NULL)))
      {
         void *state = NULL;
         state_manager_push_where(rewind_st->state, &state);

         content_serialize_state_rewind(state, rewind_st->size);

         state_manager_push_do(rewind_st->state);
      }
   }

   /* Update core audio callbacks */
   if (current_core)
   {
      if (current_core->retro_set_audio_sample)
         current_core->retro_set_audio_sample(
               (rewind_st->flags
                & STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED)
               ? audio_driver_sample_rewind
               : audio_driver_sample);

      if (current_core->retro_set_audio_sample_batch)
         current_core->retro_set_audio_sample_batch(
               (  rewind_st->flags
                & STATE_MGR_REWIND_ST_FLAG_FRAME_IS_REVERSED)
               ? audio_driver_sample_batch_rewind
               : audio_driver_sample_batch);
   }

   if (pressed)
      rewind_st->flags |=  STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED;
   else
      rewind_st->flags &= ~STATE_MGR_REWIND_ST_FLAG_HOTKEY_WAS_PRESSED;
   return ret;
}
