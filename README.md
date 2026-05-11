# cdsp_peak

Small FastRPC/QAIC benchmark for Hexagon v68/v73 CDSP.

This project is intentionally small and local-machine oriented. It assumes the
FastRPC runtime, DSP firmware, and CDSP dynamic libraries are already installed
on the target machine.

It measures a narrow first set of synthetic limits:

- `rpc-null`: host-visible FastRPC call overhead.
- `fp32-vmuladd`: HVX FP32 vector multiply plus add.
- `fp16-vmpyacc`: HVX FP16 vector multiply-accumulate.
- `qf16-vmpyadd`, `qf32-vmpyadd`: HVX qfloat multiply plus add.
- `int8-vrmpyacc`, `int8-vrmpy-add`: fixed-input diagnostic variants for
  comparing mathematically equivalent int8 accumulation forms.
- `mem-read`, `mem-write`, `mem-copy`: shared `rpcmem` DDR bandwidth.
- `hmx-resource`, `hmx-cached`, `hmx-lock`: experimental v68 HMX resource
  probes for VTCM acquire, cached acquire, and HMX lock.
- `hmx-int8-ub`, `hmx-int8-cm-ub`, `hmx-int8-uh`, and the `hmx-int8-*-x*`
  burst variants: experimental v68 HMX int8 tile instruction probes, included
  in the default `all` scenario.
- `hmx-int8-uh2x2-full-x64`: experimental v69+ HMX int8 probe that stores the
  wider `acc:2x2` unsigned-half accumulator shape. It is included in `all`
  only when the detected DSP architecture is v69 or newer.
- `hmx-int8-ub-adeep32`, `hmx-int8-ub-wdeep-full-x64`: experimental probes for
  PRM-style activation deep and weight deep HMX multiply variants, included in
  the default `all` scenario.

## Requirements

Default paths assume this sibling directory layout:

```text
hexagon-experiment/
  QAIC/
  Hexagon_open_access.Core.19.0.02.Linux-ARM64/
  cdsp_peak/
```

Required host pieces:

- FastRPC headers and libraries, normally under `/usr/include/fastrpc` and
  linkable as `-lcdsprpc`.
- `rpcmem` support and working CDSP FastRPC access.
- QAIC built from the open-source Qualcomm QAIC repository.
- LLVM/clang for the AArch64 host build.
- Hexagon Open Access tools for `hexagon-clang` and Hexagon target headers.
- `libbsd` for the generated QAIC host stub link.

The build does not require Hexagon SDK headers. The small subset of
`HAP_power_set` ABI declarations used for power voting lives in this tree.

The Makefile variables can be overridden if your tree differs:

```sh
make -C cdsp_peak \
  QAIC=/path/to/qaic \
  HEXAGON_TOOLS_ROOT=/path/to/Hexagon_open_access.Core.19.0.02.Linux-ARM64 \
  FASTRPC_INC=/path/to/fastrpc/include
```

## Build

```sh
make -C cdsp_peak
```

Install the runnable files into one directory:

```sh
make -C cdsp_peak install
./cdsp_peak/install/cdsp_peak --scenario all
```

The install directory can be overridden:

```sh
make -C cdsp_peak install INSTALL_DIR=/tmp/cdsp_peak
/tmp/cdsp_peak/cdsp_peak --scenario all
```

The `run` target accepts make/environment variables:

```sh
make -C cdsp_peak run
THREADS=1 make -C cdsp_peak run
SCENARIO=qf32-vmpyadd THREADS=2 RUN_ARGS="--min-ms 100 --size-mib 1" make -C cdsp_peak run
```

Useful options:

```sh
./cdsp_peak/build/host/cdsp_peak --scenario fp16-vmpyacc,qf16-vmpyadd,int8-vrmpyacc --min-ms 500
./cdsp_peak/build/host/cdsp_peak --scenario int8-vrmpyacc,int8-vrmpy-add --iterations 1000000
./cdsp_peak/build/host/cdsp_peak --scenario qf32-vmpyadd,int8-vrmpy-add --threads 2
./cdsp_peak/build/host/cdsp_peak --scenario mem-copy --size-mib 128
./cdsp_peak/build/host/cdsp_peak --scenario hmx-resource,hmx-cached,hmx-lock --iterations 1
timeout 10s ./cdsp_peak/build/host/cdsp_peak --scenario hmx-int8-cm-ub --iterations 1
timeout 60s ./cdsp_peak/build/host/cdsp_peak --scenario hmx-int8-ub-full-x64 --min-ms 300 --size-mib 1
timeout 60s ./cdsp_peak/build/host/cdsp_peak --scenario hmx-int8-uh2x2-full-x64 --min-ms 300 --size-mib 1
timeout 60s ./cdsp_peak/build/host/cdsp_peak --scenario hmx-int8-ub-adeep32,hmx-int8-ub-wdeep-full-x64 --min-ms 300 --size-mib 1
./cdsp_peak/build/host/cdsp_peak --scenario all --power none
./cdsp_peak/build/host/cdsp_peak --reset
```

`make install` copies `cdsp_peak`, `libcdsp_peak_stub.so`, and
`libcdsp_peak_skel_v68.so`/`libcdsp_peak_skel_v73.so` into `INSTALL_DIR`. The
executable has `$ORIGIN` rpath for the host stub, and at startup it sets
`ADSP_LIBRARY_PATH` and `DSP_LIBRARY_PATH` to its own directory when skel
libraries are installed beside the executable. This avoids FastRPC multi-path
lookup quirks.

`make clean` removes generated QAIC sources, objects, binaries, and DSP shared
objects under `build/`. It does not remove `INSTALL_DIR`.

## Notes

The build emits both v68 and v73 DSP skel libraries. At startup, the host uses
`DSPRPC_GET_DSP_INFO`/`ARCH_VER` to select `libcdsp_peak_skel_v68.so` or
`libcdsp_peak_skel_v73.so`; HMX scenarios that need newer HMX encodings can
declare a minimum default architecture, so `--scenario all` skips
`hmx-int8-uh2x2-full-x64` on v68 while still allowing it to be requested
explicitly. `mem-copy` reports touched bytes as read plus written bytes, so its
GB/s number is twice the copied payload size. When `--threads` is omitted,
`cdsp_peak` uses the FastRPC-reported `HVX_SUPPORT_128B` count as the default
thread count. `--threads` opens one FastRPC handle per host thread and gives
each memory thread its own `--size-mib` source and destination buffers. The
`count` column is total calls for `rpc-null`, total iterations for compute
scenarios, and total repeats for memory scenarios. Enabled int8 scenarios share
one per-thread iteration count so their checksums are directly comparable. Use
`--threads` inside one `cdsp_peak` process; running multiple benchmark
processes against the same CDSP domain concurrently can leave FastRPC open calls
blocked until the domain recovers or is reset externally.

The banner prints FastRPC-reported capability fields such as VTCM page/count and
HMX depth/spatial support. These are advisory capability values, not proof that
every HMX tile instruction sequence is usable.

The default power mode is `--power max`, matching the llama.cpp Hexagon path:
the DSP code votes for compute client class, DCVS performance mode with max
core and bus corners, sleep disable, HVX power-up, and HMX power-up. Use
`--power none` for baseline runs without these HAP power votes. The banner
prints `power=` and `power_mask=` so results record which votes were applied.

## HMX Status

The HMX rows are reverse-engineering probes, not a stable benchmark yet. They
compile the DSP-side HMX path with `-mhmx`, weak-link the `compute_resource_*`
runtime symbols used by QNN/Hexagon SDK code, stage tiles in VTCM, and use Open
Access intrinsics for `mxclracc`, `bias = mxmem2(...)`,
`activation.ub = mxmem(...)`, `weight.b = mxmem(...)`, and direct `acc` stores.
On this v68 target, with the default `--power max` HAP votes, the resource,
cached acquire, HMX lock, and current int8 tile probes return, so HMX is part of
the default `all` scenario. These instruction layouts are still experimental;
use `timeout` plus `--reset` while iterating on layouts and instruction counts.

The simple `hmx-int8-ub`/`hmx-int8-uh` rows store one HMX half tile and use a
conservative `32 x 32 x 32 x 2` int8-op count per activation/weight packet.
The `hmx-int8-ub-full-x16`, `hmx-int8-ub-full-x32`, and
`hmx-int8-ub-full-x64` rows keep accumulating on one HMX accumulator, then
store both `before` and `after` `sat.ub` halves with `before:retain` plus
`after`. They use the FastRPC-reported v68 `hmx_depth=32` and
`hmx_spatial=64` shape, so each activation/weight packet is counted as
`32 x 64 x 32 x 2` int8 ops. On the current v68 target,
`hmx-int8-ub-full-x64` reaches about 8.0k ops/cycle and 11.5 TOPS with the
default `--power max` votes, which is close to the 8192 ops/cycle ideal for a
single v68 HMX issue stream. The remaining gap to a 12 TOPS marketing number is
consistent with the observed runtime clock rather than an obviously missing
packet pattern.

On v69+ HMX headers expose `acc:2x2` unsigned-half accumulator stores. The
`hmx-int8-uh2x2-full-x64` row uses the same 64 activation/weight issue stream as
`hmx-int8-ub-full-x64`, but stores the wider `before` and `after`
`sat.uh=acc:2x2` result. It counts this as a `32 x 128 x 32 x 2` int8 shape per
activation/weight packet. On a tested v73 target this row reports roughly 50
TOPS with default power votes, which is near the advertised 45 TOPS class, but
the row should still be treated as an instruction probe until the exact public
HMX tile-shape documentation is available.

The `hmx-int8-ub-adeep32` and `hmx-int8-ub-wdeep-full-x64` rows exercise
PRM-style multiply variants:
`activation.ub=mxmem(...):deep` for 32 contiguous int8 croutons, and
`weight.b=mxmem(...):deep` for 64 filters. On the same tested v73 target they
run correctly but score lower than the `acc:2x2` path, roughly 13 TOPS and
22 TOPS respectively with the current synthetic layout. This suggests that the
advertised 45 TOPS-class figure is not explained by simply switching the int8
probe to activation-deep or weight-deep form.

The currently generated HMX instruction packets can be inspected with:

```sh
make -C cdsp_peak inspect INSPECT_ARCH=v68
../Hexagon_open_access.Core.19.0.02.Linux-ARM64/Tools/bin/hexagon-llvm-objdump \
  -d --mcpu=hexagonv68 --mattr=+hmx,+hmxv68 \
  cdsp_peak/build/dsp/v68/cdsp_peak_hmx.o
```

For v73 output use `INSPECT_ARCH=v73`; the 2x2 path should disassemble to
`mxmem(...):before:retain:sat.uh = acc:2x2` and
`mxmem(...):after:sat.uh = acc:2x2` stores.
The PRM-aligned experiments should show `activation.ub = mxmem(...):deep` and
`weight.b = mxmem(...):deep` packets.
