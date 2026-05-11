#pragma once

#ifdef _WIN32
#include <BaseTsd.h>
#include <io.h>
#include <stddef.h>

typedef SSIZE_T ssize_t;

#ifndef R_OK
#define R_OK 4
#endif

#define access _access

ssize_t cdsp_peak_readlink(const char *path, char *buf, size_t bufsiz);
#define readlink cdsp_peak_readlink
#else
#include_next <unistd.h>
#endif
