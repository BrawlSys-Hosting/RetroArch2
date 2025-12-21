/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"

#include <mach/mach_time.h>
#include <stdint.h>
#include <strings.h>

uint32 Platform::GetCurrentTimeMS()
{
   static mach_timebase_info_data_t timebase = { 0, 0 };
   uint64_t now = mach_absolute_time();

   if (timebase.denom == 0) {
      (void)mach_timebase_info(&timebase);
   }

   uint64_t nanos = now * timebase.numer / timebase.denom;
   return (uint32)(nanos / 1000000);
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
