#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <errno.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>

typedef HANDLE pthread_t;

struct cdsp_peak_pthread_start {
   void *(*routine)(void *);
   void *arg;
};

static unsigned __stdcall cdsp_peak_pthread_trampoline(void *data)
{
   struct cdsp_peak_pthread_start *start =
      (struct cdsp_peak_pthread_start *)data;
   void *(*routine)(void *) = start->routine;
   void *arg = start->arg;

   free(start);
   (void)routine(arg);
   return 0;
}

static int pthread_create(pthread_t *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg)
{
   struct cdsp_peak_pthread_start *start;
   uintptr_t handle;

   (void)attr;
   if (!thread || !start_routine)
      return EINVAL;

   start = (struct cdsp_peak_pthread_start *)malloc(sizeof(*start));
   if (!start)
      return ENOMEM;
   start->routine = start_routine;
   start->arg = arg;

   handle = _beginthreadex(NULL, 0, cdsp_peak_pthread_trampoline, start, 0, NULL);
   if (!handle) {
      int err = errno ? errno : EAGAIN;
      free(start);
      return err;
   }

   *thread = (HANDLE)handle;
   return 0;
}

static int pthread_join(pthread_t thread, void **retval)
{
   if (retval)
      *retval = NULL;
   if (!thread)
      return EINVAL;
   WaitForSingleObject(thread, INFINITE);
   CloseHandle(thread);
   return 0;
}
#else
#include_next <pthread.h>
#endif
