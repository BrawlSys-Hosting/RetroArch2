/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"

#include <strings.h>
#include <time.h>

uint32 Platform::GetCurrentTimeMS()
{
   struct timespec current;
   clock_gettime(CLOCK_MONOTONIC, &current);

   return (uint32)((current.tv_sec * 1000) + (current.tv_nsec / 1000000));
}

int Platform::GetConfigInt(const char* name)
{
   const char *value = getenv(name);
   if (!value) {
      return 0;
   }
   return atoi(value);
}

bool Platform::GetConfigBool(const char* name)
{
   const char *value = getenv(name);
   if (!value) {
      return false;
   }
   return atoi(value) != 0 || strcasecmp(value, "true") == 0;
}

