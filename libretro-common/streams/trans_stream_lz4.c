/* Copyright  (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (trans_stream_lz4.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>

#include <string/stdstring.h>
#include <streams/trans_stream.h>

#include <lz4.h>

struct lz4_trans_stream
{
   const uint8_t *in;
   uint32_t in_size;
   uint8_t *out;
   uint32_t out_size;
   int accel;
};

static void *lz4_stream_new(void)
{
   struct lz4_trans_stream *ret = (struct lz4_trans_stream*)
      calloc(1, sizeof(*ret));
   if (!ret)
      return NULL;
   ret->accel = 1;
   return ret;
}

static void lz4_stream_free(void *data)
{
   free(data);
}

static bool lz4_compress_define(void *data, const char *prop, uint32_t val)
{
   struct lz4_trans_stream *lz = (struct lz4_trans_stream*)data;
   int accel;

   if (!lz || !prop)
      return false;

   if (!string_is_equal(prop, "accel"))
      return false;

   accel = (int)val;
   if (accel < 1)
      accel = 1;
   lz->accel = accel;
   return true;
}

static void lz4_set_in(void *data, const uint8_t *in, uint32_t in_size)
{
   struct lz4_trans_stream *lz = (struct lz4_trans_stream*)data;

   if (!lz)
      return;

   lz->in = in;
   lz->in_size = in_size;
}

static void lz4_set_out(void *data, uint8_t *out, uint32_t out_size)
{
   struct lz4_trans_stream *lz = (struct lz4_trans_stream*)data;

   if (!lz)
      return;

   lz->out = out;
   lz->out_size = out_size;
}

static bool lz4_compress_trans(
      void *data, bool flush,
      uint32_t *rd, uint32_t *wn,
      enum trans_stream_error *err)
{
   struct lz4_trans_stream *lz = (struct lz4_trans_stream*)data;
   int compressed = 0;

   (void)flush;

   if (rd)
      *rd = 0;
   if (wn)
      *wn = 0;

   if (!lz || !lz->out)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_INVALID;
      return false;
   }

   if (!lz->in_size)
      return true;

   if (lz->in_size > (uint32_t)LZ4_MAX_INPUT_SIZE)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_INVALID;
      return false;
   }

   compressed = LZ4_compress_fast((const char*)lz->in, (char*)lz->out,
         (int)lz->in_size, (int)lz->out_size, lz->accel);
   if (compressed <= 0)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_BUFFER_FULL;
      return false;
   }

   if (rd)
      *rd = lz->in_size;
   if (wn)
      *wn = (uint32_t)compressed;
   return true;
}

static bool lz4_decompress_trans(
      void *data, bool flush,
      uint32_t *rd, uint32_t *wn,
      enum trans_stream_error *err)
{
   struct lz4_trans_stream *lz = (struct lz4_trans_stream*)data;
   int decoded = 0;

   (void)flush;

   if (rd)
      *rd = 0;
   if (wn)
      *wn = 0;

   if (!lz || !lz->out)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_INVALID;
      return false;
   }

   if (!lz->in_size)
      return true;

   decoded = LZ4_decompress_safe((const char*)lz->in, (char*)lz->out,
         (int)lz->in_size, (int)lz->out_size);
   if (decoded < 0)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_INVALID;
      return false;
   }

   if (rd)
      *rd = lz->in_size;
   if (wn)
      *wn = (uint32_t)decoded;
   return true;
}

const struct trans_stream_backend lz4_compress_backend = {
   "lz4_compress",
   &lz4_decompress_backend,
   lz4_stream_new,
   lz4_stream_free,
   lz4_compress_define,
   lz4_set_in,
   lz4_set_out,
   lz4_compress_trans
};

const struct trans_stream_backend lz4_decompress_backend = {
   "lz4_decompress",
   &lz4_compress_backend,
   lz4_stream_new,
   lz4_stream_free,
   NULL,
   lz4_set_in,
   lz4_set_out,
   lz4_decompress_trans
};
