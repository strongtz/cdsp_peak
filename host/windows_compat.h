#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <sal.h>
#include <time.h>

#ifndef STATIC_LIB
#define STATIC_LIB 1
#endif

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 1
#endif

int cdsp_peak_clock_gettime(int clock_id, struct timespec *ts);
int cdsp_peak_setenv(const char *name, const char *value, int overwrite);

#define clock_gettime cdsp_peak_clock_gettime
#define setenv cdsp_peak_setenv
#endif
