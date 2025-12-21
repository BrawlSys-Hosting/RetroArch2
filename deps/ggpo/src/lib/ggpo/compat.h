#ifndef GGPO_COMPAT_H
#define GGPO_COMPAT_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef __cdecl
#define __cdecl
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef va_copy
#define va_copy(dest, src) ((dest) = (src))
#endif

#if !defined(_MSC_VER)
static inline int sprintf_s(char *buffer, size_t size, const char *fmt, ...)
{
   int result;
   va_list args;

   va_start(args, fmt);
   result = vsnprintf(buffer, size, fmt, args);
   va_end(args);

   return result;
}

static inline int vsprintf_s(char *buffer, size_t size, const char *fmt, va_list args)
{
   return vsnprintf(buffer, size, fmt, args);
}

static inline int strcpy_s(char *dest, const char *src)
{
   if (!dest || !src) {
      return EINVAL;
   }
   strcpy(dest, src);
   return 0;
}

static inline int strcpy_s(char *dest, size_t destsz, const char *src)
{
   if (!dest || !src) {
      return EINVAL;
   }
   if (destsz == 0) {
      return EINVAL;
   }
   strncpy(dest, src, destsz - 1);
   dest[destsz - 1] = '\0';
   return 0;
}

static inline int strncat_s(char *dest, size_t destsz, const char *src, size_t count)
{
   size_t to_copy;

   if (!dest || !src) {
      return EINVAL;
   }
   if (destsz == 0) {
      return EINVAL;
   }

   to_copy = count;
   if (to_copy > destsz - 1) {
      to_copy = destsz - 1;
   }

   strncat(dest, src, to_copy);
   dest[strlen(dest)] = '\0';
   return 0;
}

static inline int fopen_s(FILE **fp, const char *filename, const char *mode)
{
   FILE *file = fopen(filename, mode);
   if (!file) {
      if (fp) {
         *fp = NULL;
      }
      return errno;
   }
   *fp = file;
   return 0;
}
#endif

#endif
