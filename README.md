# cdsp_peak

Small FastRPC/QAIC benchmark for Hexagon v68 CDSP.

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

## Requirements

Default paths assume this sibling directory layout:

```text
hexagon-experiment/
  QAIC/
  Hexagon_open_access.Core.19.0.02.Linux-ARM64/
  Hexagon_SDK/6.5.0.0/
  cdsp_peak/
```

Required host pieces:

- FastRPC headers and libraries, normally under `/usr/include/fastrpc` and
  linkable as `-lcdsprpc`.
- `rpcmem` support and working CDSP FastRPC access.
- QAIC built from the open-source Qualcomm QAIC repository.
- LLVM/clang for the AArch64 host build.
- Hexagon Open Access tools for `hexagon-clang` and Hexagon target headers.
- Hexagon SDK headers for the DSP power vote interface. The build uses the SDK
  only as an include source for `HAP_power.h`; the DSP compiler still comes
  from the Open Access toolchain.
- `libbsd` for the generated QAIC host stub link.

The Makefile variables can be overridden if your tree differs:

```sh
make -C cdsp_peak \
  QAIC=/path/to/qaic \
  HEXAGON_TOOLS_ROOT=/path/to/Hexagon_open_access.Core.19.0.02.Linux-ARM64 \
  HEXAGON_SDK_ROOT=/path/to/Hexagon_SDK/6.5.0.0 \
  FASTRPC_INC=/path/to/fastrpc/include
```

## Build

```sh
make -C cdsp_peak
```

Run all scenarios:

```sh
LD_LIBRARY_PATH="$PWD/cdsp_peak/build/host" \
ADSP_LIBRARY_PATH="$PWD/cdsp_peak/build/dsp/v68" \
DSP_LIBRARY_PATH="$PWD/cdsp_peak/build/dsp/v68" \
./cdsp_peak/build/host/cdsp_peak --scenario all
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
./cdsp_peak/build/host/cdsp_peak --scenario all --power none
./cdsp_peak/build/host/cdsp_peak --reset
```

`make clean` removes generated QAIC sources, objects, binaries, and DSP shared
objects under `build/`.

## Notes

The first version is intentionally v68-only. `mem-copy` reports touched bytes
as read plus written bytes, so its GB/s number is twice the copied payload size.
When `--threads` is omitted, `cdsp_peak` uses the FastRPC-reported
`HVX_SUPPORT_128B` count as the default thread count. `--threads` opens one
FastRPC handle per host thread and gives each memory thread its own
`--size-mib` source and destination buffers. The `count` column is total calls
for `rpc-null`, total iterations for compute scenarios, and total repeats for
memory scenarios. Enabled int8 scenarios share one per-thread iteration count so
their checksums are directly comparable. Use `--threads` inside one `cdsp_peak`
process; running multiple benchmark processes against the same CDSP domain
concurrently can leave FastRPC open calls blocked until the domain recovers or
is reset externally.

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
`32 x 64 x 32 x 2` int8 ops. On the current target, `hmx-int8-ub-full-x64`
reaches about 8.0k ops/cycle and 11.5 TOPS with the default `--power max`
votes, which is close to the 8192 ops/cycle ideal for a single v68 HMX issue
stream. The remaining gap to a 12 TOPS marketing number is consistent with the
observed runtime clock rather than an obviously missing packet pattern.

The currently generated HMX instruction packets can be inspected with:

```sh
../Hexagon_open_access.Core.19.0.02.Linux-ARM64/Tools/bin/hexagon-llvm-objdump \
  -d --mcpu=hexagonv68 --mattr=+hmx,+hmxv68 \
  cdsp_peak/build/dsp/v68/cdsp_peak_hmx.o
```
