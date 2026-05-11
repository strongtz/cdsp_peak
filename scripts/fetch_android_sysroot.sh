#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

out_dir=${1:-build/android/sysroot}
adb_bin=${ADB:-adb}

if ! command -v "$adb_bin" >/dev/null 2>&1; then
   echo "adb not found; set ADB=/path/to/adb or install Android platform-tools" >&2
   exit 127
fi

find_remote() {
   "$adb_bin" shell sh -c 'for p in "$@"; do [ -r "$p" ] && { echo "$p"; exit 0; }; done; exit 1' sh "$@" |
      tr -d '\r' |
      sed -n '1p'
}

pull_first() {
   name=$1
   dest=$2
   shift 2

   remote=$(find_remote "$@") || {
      echo "missing Android library for $name" >&2
      echo "looked at:" >&2
      for p in "$@"; do
         echo "  $p" >&2
      done
      exit 1
   }

   mkdir -p "$(dirname "$dest")"
   echo "pull $remote -> $dest"
   "$adb_bin" pull "$remote" "$dest" >/dev/null
}

mkdir -p "$out_dir/system/lib64" "$out_dir/vendor/lib64"

runtime_paths() {
   lib=$1
   printf '%s\n' \
      "/apex/com.android.runtime/lib64/bionic/$lib" \
      "/system/lib64/$lib"
}

pull_first libc.so "$out_dir/system/lib64/libc.so" $(runtime_paths libc.so)
pull_first libdl.so "$out_dir/system/lib64/libdl.so" $(runtime_paths libdl.so)
pull_first libm.so "$out_dir/system/lib64/libm.so" $(runtime_paths libm.so)
pull_first liblog.so "$out_dir/system/lib64/liblog.so" \
   "/system/lib64/liblog.so" \
   "/apex/com.android.runtime/lib64/bionic/liblog.so"

pull_first libcdsprpc.so "$out_dir/vendor/lib64/libcdsprpc.so" \
   "/vendor/lib64/libcdsprpc.so" \
   "/system/vendor/lib64/libcdsprpc.so" \
   "/odm/lib64/libcdsprpc.so" \
   "/system/lib64/libcdsprpc.so"

touch "$out_dir/.stamp"
