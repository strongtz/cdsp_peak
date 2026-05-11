// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "AEEStdErr.h"
#include "remote.h"
#include "rpcmem.h"

typedef int (*remote_handle64_open_fn)(const char *name, remote_handle64 *ph);
typedef int (*remote_handle64_invoke_fn)(remote_handle64 h, uint32_t dwScalars,
                                         remote_arg *pra);
typedef int (*remote_handle64_close_fn)(remote_handle64 h);
typedef int (*remote_handle_control_fn)(uint32_t req, void *data,
                                        uint32_t datalen);
typedef int (*remote_handle64_control_fn)(remote_handle64 h, uint32_t req,
                                          void *data, uint32_t datalen);
typedef int (*remote_session_control_fn)(uint32_t req, void *data,
                                         uint32_t datalen);
typedef void *(*rpcmem_alloc_fn)(int heapid, uint32 flags, int size);
typedef void *(*rpcmem_alloc2_fn)(int heapid, uint32 flags, size_t size);
typedef void (*rpcmem_free_fn)(void *po);
typedef int (*rpcmem_to_fd_fn)(void *po);

static HMODULE g_cdsprpc;
static int g_init_status = AEE_EFAILED;

static remote_handle64_open_fn g_remote_handle64_open;
static remote_handle64_invoke_fn g_remote_handle64_invoke;
static remote_handle64_close_fn g_remote_handle64_close;
static remote_handle_control_fn g_remote_handle_control;
static remote_handle64_control_fn g_remote_handle64_control;
static remote_session_control_fn g_remote_session_control;
static rpcmem_alloc_fn g_rpcmem_alloc;
static rpcmem_alloc2_fn g_rpcmem_alloc2;
static rpcmem_free_fn g_rpcmem_free;
static rpcmem_to_fd_fn g_rpcmem_to_fd;

static void print_last_error(const char *what, const wchar_t *path)
{
   char narrow[MAX_PATH * 4];
   DWORD err = GetLastError();

   narrow[0] = '\0';
   if (path) {
      WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), NULL,
                          NULL);
   }
   fprintf(stderr, "%s%s%s failed: Windows error %lu\n", what,
           path ? " " : "", path ? narrow : "", err);
}

static int utf8_to_wide(const char *src, wchar_t *dst, size_t dst_len)
{
   int written;

   if (!src || !dst || dst_len == 0)
      return -1;
   written = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_len);
   return written > 0 ? 0 : -1;
}

static HMODULE try_load_path(const wchar_t *path)
{
   HMODULE module = LoadLibraryW(path);

   if (!module)
      print_last_error("LoadLibraryW", path);
   return module;
}

static HMODULE load_from_env(void)
{
   char *path = NULL;
   size_t len = 0;
   wchar_t wide[MAX_PATH * 4];
   HMODULE module = NULL;

   if (_dupenv_s(&path, &len, "CDSPRPC_DLL") || !path)
      return NULL;

   if (utf8_to_wide(path, wide, sizeof(wide) / sizeof(wide[0])) == 0)
      module = try_load_path(wide);
   free(path);
   return module;
}

static HMODULE load_from_service(const wchar_t *service_name)
{
   SC_HANDLE scm = NULL;
   SC_HANDLE service = NULL;
   LPQUERY_SERVICE_CONFIGW config = NULL;
   DWORD needed = 0;
   wchar_t dll_path[MAX_PATH * 4];
   wchar_t *slash;
   HMODULE module = NULL;

   scm = OpenSCManagerW(NULL, NULL, STANDARD_RIGHTS_READ);
   if (!scm)
      goto out;

   service = OpenServiceW(scm, service_name, SERVICE_QUERY_CONFIG);
   if (!service)
      goto out;

   if (QueryServiceConfigW(service, NULL, 0, &needed) ||
       GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      goto out;

   config = (LPQUERY_SERVICE_CONFIGW)LocalAlloc(LMEM_FIXED, needed);
   if (!config)
      goto out;

   if (!QueryServiceConfigW(service, config, needed, &needed))
      goto out;

   wcsncpy_s(dll_path, sizeof(dll_path) / sizeof(dll_path[0]),
             config->lpBinaryPathName, _TRUNCATE);
   if (!wcsncmp(dll_path, L"\\SystemRoot", 11)) {
      wchar_t windir[MAX_PATH];

      if (GetEnvironmentVariableW(L"windir", windir,
                                  sizeof(windir) / sizeof(windir[0])) > 0) {
         wchar_t replaced[MAX_PATH * 4];

         swprintf_s(replaced, sizeof(replaced) / sizeof(replaced[0]), L"%ls%ls",
                    windir, dll_path + 11);
         wcsncpy_s(dll_path, sizeof(dll_path) / sizeof(dll_path[0]),
                   replaced, _TRUNCATE);
      }
   }

   slash = wcsrchr(dll_path, L'\\');
   if (!slash)
      goto out;
   slash[1] = L'\0';
   wcsncat_s(dll_path, sizeof(dll_path) / sizeof(dll_path[0]),
             L"libcdsprpc.dll", _TRUNCATE);

   module = LoadLibraryW(dll_path);

out:
   if (config)
      LocalFree(config);
   if (service)
      CloseServiceHandle(service);
   if (scm)
      CloseServiceHandle(scm);
   return module;
}

static int load_symbol(FARPROC *slot, const char *name)
{
   *slot = GetProcAddress(g_cdsprpc, name);
   if (!*slot) {
      fprintf(stderr, "GetProcAddress(%s) failed: Windows error %lu\n", name,
              GetLastError());
      return AEE_EUNABLETOLOAD;
   }
   return AEE_SUCCESS;
}

static int ensure_cdsprpc_loaded(void)
{
   static int initialized;
   int err;

   if (initialized)
      return g_init_status;

   initialized = 1;
   g_cdsprpc = load_from_env();
   if (!g_cdsprpc)
      g_cdsprpc = load_from_service(L"qcnspmcdm");
   if (!g_cdsprpc)
      g_cdsprpc = load_from_service(L"qcadsprpc");
   if (!g_cdsprpc)
      g_cdsprpc = LoadLibraryW(L"libcdsprpc.dll");
   if (!g_cdsprpc) {
      fprintf(stderr,
              "Unable to load libcdsprpc.dll. Set CDSPRPC_DLL to the driver "
              "copy if the Qualcomm service path cannot be discovered.\n");
      g_init_status = AEE_EUNABLETOLOAD;
      return g_init_status;
   }

#define LOAD_REQUIRED(fn)                                                     \
   do {                                                                       \
      err = load_symbol((FARPROC *)&g_##fn, #fn);                             \
      if (err) {                                                              \
         g_init_status = err;                                                 \
         return g_init_status;                                                \
      }                                                                       \
   } while (0)

   LOAD_REQUIRED(remote_handle64_open);
   LOAD_REQUIRED(remote_handle64_invoke);
   LOAD_REQUIRED(remote_handle64_close);
   LOAD_REQUIRED(remote_handle_control);
   LOAD_REQUIRED(remote_handle64_control);
   LOAD_REQUIRED(remote_session_control);
   LOAD_REQUIRED(rpcmem_alloc);
   LOAD_REQUIRED(rpcmem_free);
   LOAD_REQUIRED(rpcmem_to_fd);

#undef LOAD_REQUIRED

   g_rpcmem_alloc2 = (rpcmem_alloc2_fn)GetProcAddress(g_cdsprpc, "rpcmem_alloc2");
   g_init_status = AEE_SUCCESS;
   return g_init_status;
}

void rpcmem_init(void)
{
   (void)ensure_cdsprpc_loaded();
}

void rpcmem_deinit(void)
{
}

void *rpcmem_alloc(int heapid, uint32 flags, int size)
{
   if (ensure_cdsprpc_loaded())
      return NULL;
   return g_rpcmem_alloc(heapid, flags, size);
}

void *rpcmem_alloc2(int heapid, uint32 flags, size_t size)
{
   if (ensure_cdsprpc_loaded())
      return NULL;
   if (g_rpcmem_alloc2)
      return g_rpcmem_alloc2(heapid, flags, size);
   if (size > INT32_MAX)
      return NULL;
   return g_rpcmem_alloc(heapid, flags, (int)size);
}

void rpcmem_free(void *po)
{
   if (!ensure_cdsprpc_loaded())
      g_rpcmem_free(po);
}

int rpcmem_to_fd(void *po)
{
   if (ensure_cdsprpc_loaded())
      return -1;
   return g_rpcmem_to_fd(po);
}

int remote_handle64_open(const char *name, remote_handle64 *ph)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_handle64_open(name, ph);
}

int remote_handle64_invoke(remote_handle64 h, uint32_t dwScalars,
                           remote_arg *pra)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_handle64_invoke(h, dwScalars, pra);
}

int remote_handle64_close(remote_handle64 h)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_handle64_close(h);
}

int remote_handle_control(uint32_t req, void *data, uint32_t datalen)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_handle_control(req, data, datalen);
}

int remote_handle64_control(remote_handle64 h, uint32_t req, void *data,
                            uint32_t datalen)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_handle64_control(h, req, data, datalen);
}

int remote_session_control(uint32_t req, void *data, uint32_t datalen)
{
   int err = ensure_cdsprpc_loaded();

   if (err)
      return err;
   return g_remote_session_control(req, data, datalen);
}
#endif
