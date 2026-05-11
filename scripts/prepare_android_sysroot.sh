#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

out_dir=${1:-build/android/sysroot}
clang_bin=${2:-clang}
target=${3:-aarch64-linux-android29}
fastrpc_name=${4:-libcdsprpc.so}
fastrpc_lib=${5:-}

if ! command -v "$clang_bin" >/dev/null 2>&1; then
   echo "clang not found: $clang_bin" >&2
   exit 127
fi

mkdir -p "$out_dir/system/lib64" "$out_dir/vendor/lib64"

for lib in libc.so libdl.so libm.so liblog.so; do
   "$clang_bin" --target="$target" -fuse-ld=lld -nostdlib -shared \
      -Wl,-soname,"$lib" \
      -o "$out_dir/system/lib64/$lib" \
      -x c /dev/null
done

if [ -n "$fastrpc_lib" ]; then
   if [ ! -r "$fastrpc_lib" ]; then
      echo "Android FastRPC library not found: $fastrpc_lib" >&2
      echo "Set ANDROID_FASTRPC_LIB=/path/to/$fastrpc_name" >&2
      exit 1
   fi
   install -m 0755 "$fastrpc_lib" "$out_dir/vendor/lib64/$fastrpc_name"
else
   "$clang_bin" --target="$target" -fuse-ld=lld -nostdlib -shared \
      -Wl,-soname,"$fastrpc_name" \
      -o "$out_dir/vendor/lib64/$fastrpc_name" \
      -x c - <<'EOF'
#include <stddef.h>
#include <stdint.h>

typedef uint32_t remote_handle;
typedef uint64_t remote_handle64;

typedef struct {
   void *pv;
   size_t nLen;
} remote_buf;

typedef union {
   remote_buf buf;
   remote_handle h;
   remote_handle64 h64;
} remote_arg;

int remote_handle_open(const char *name, remote_handle *ph)
{
   (void)name;
   (void)ph;
   return -1;
}

int remote_handle64_open(const char *name, remote_handle64 *ph)
{
   (void)name;
   (void)ph;
   return -1;
}

int remote_handle64_invoke(remote_handle64 h, uint32_t scalars,
                           remote_arg *args)
{
   (void)h;
   (void)scalars;
   (void)args;
   return -1;
}

int remote_handle64_close(remote_handle64 h)
{
   (void)h;
   return -1;
}

int remote_session_control(uint32_t req, void *data, uint32_t datalen)
{
   (void)req;
   (void)data;
   (void)datalen;
   return -1;
}

void rpcmem_init(void) {}
void rpcmem_deinit(void) {}

void *rpcmem_alloc(int heapid, uint32_t flags, int size)
{
   (void)heapid;
   (void)flags;
   (void)size;
   return NULL;
}

void *rpcmem_alloc2(int heapid, uint32_t flags, size_t size)
{
   (void)heapid;
   (void)flags;
   (void)size;
   return NULL;
}

void rpcmem_free(void *ptr)
{
   (void)ptr;
}

int rpcmem_to_fd(void *ptr)
{
   (void)ptr;
   return -1;
}
EOF
fi

touch "$out_dir/.stamp"
