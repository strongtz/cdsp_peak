// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "unistd.h"

int cdsp_peak_clock_gettime(int clock_id, struct timespec *ts)
{
   static LARGE_INTEGER frequency;
   static int initialized;
   LARGE_INTEGER counter;
   long long seconds;
   long long remainder;
   long long nanoseconds;

   (void)clock_id;
   if (!ts)
      return -1;

   if (!initialized) {
      if (!QueryPerformanceFrequency(&frequency))
         return -1;
      initialized = 1;
   }
   if (!QueryPerformanceCounter(&counter))
      return -1;

   seconds = counter.QuadPart / frequency.QuadPart;
   remainder = counter.QuadPart % frequency.QuadPart;
   nanoseconds = (remainder * 1000000000LL) / frequency.QuadPart;

   ts->tv_sec = (time_t)seconds;
   ts->tv_nsec = (long)nanoseconds;
   return 0;
}

int cdsp_peak_setenv(const char *name, const char *value, int overwrite)
{
   char *existing;
   size_t existing_len = 0;

   if (!name || !name[0] || strchr(name, '='))
      return EINVAL;

   if (!overwrite) {
      if (_dupenv_s(&existing, &existing_len, name) == 0 && existing) {
         free(existing);
         return 0;
      }
   }

   return _putenv_s(name, value ? value : "");
}

ssize_t cdsp_peak_readlink(const char *path, char *buf, size_t bufsiz)
{
   DWORD len;

   if (!path || !buf || bufsiz == 0) {
      errno = EINVAL;
      return -1;
   }
   if (strcmp(path, "/proc/self/exe")) {
      errno = ENOENT;
      return -1;
   }

   len = GetModuleFileNameA(NULL, buf, (DWORD)bufsiz);
   if (len == 0) {
      errno = EINVAL;
      return -1;
   }
   if ((size_t)len >= bufsiz) {
      errno = ENAMETOOLONG;
      return -1;
   }

   for (DWORD i = 0; i < len; ++i) {
      if (buf[i] == '\\')
         buf[i] = '/';
   }

   return (ssize_t)len;
}
#endif
