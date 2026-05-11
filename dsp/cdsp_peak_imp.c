// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

#include "AEEStdErr.h"
#include "cdsp_peak.h"

#ifndef CDSP_PEAK_ARCH
#error "CDSP_PEAK_ARCH must be defined by the DSP build flags"
#endif
#define CDSP_PEAK_KERNEL_FP32 0
#define CDSP_PEAK_KERNEL_FP16 1
#define CDSP_PEAK_KERNEL_INT8_FIXED 2
#define CDSP_PEAK_KERNEL_INT8_ADD_FIXED 3
#define CDSP_PEAK_KERNEL_QF16 4
#define CDSP_PEAK_KERNEL_QF32 5

struct cdsp_peak_state {
   uint64_t timer_overhead;
};

static struct cdsp_peak_state g_state;
static HVX_Vector g_sink[8] __attribute__((aligned(128)));

static inline uint64_t read_cycles(void)
{
   uint64_t cycles;
   __asm__ volatile("%0 = c15:14" : "=r"(cycles));
   return cycles;
}

static uint64_t measure_timer_overhead(void)
{
   uint64_t best = UINT64_MAX;

   for (int i = 0; i < 128; ++i) {
      uint64_t start = read_cycles();
      uint64_t end = read_cycles();
      uint64_t delta = end - start;

      if (delta < best)
         best = delta;
   }

   return best == UINT64_MAX ? 0 : best;
}

static inline uint64_t elapsed_cycles(uint64_t start, uint64_t end)
{
   uint64_t delta = end - start;

   return delta > g_state.timer_overhead ? delta - g_state.timer_overhead : delta;
}

static inline HVX_Vector splat_u32(uint32_t value)
{
   return Q6_V_vsplat_R((int)value);
}

#define KEEP_VECTORS4(a, b, c, d) \
   __asm__ volatile("" : "+v"(a), "+v"(b), "+v"(c), "+v"(d))

static uint64_t checksum_vector(HVX_Vector value)
{
   uint32_t words[sizeof(HVX_Vector) / sizeof(uint32_t)] __attribute__((aligned(128)));
   uint64_t sum = 0xcbf29ce484222325ULL;

   *(HVX_Vector *)words = value;
   for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
      sum ^= (uint64_t)words[i] + ((uint64_t)i << 32);
      sum *= 0x100000001b3ULL;
   }

   return sum;
}

static uint64_t checksum_accumulators(HVX_Vector a0, HVX_Vector a1, HVX_Vector a2,
                                      HVX_Vector a3, HVX_Vector a4, HVX_Vector a5,
                                      HVX_Vector a6, HVX_Vector a7)
{
   HVX_Vector sum0 = Q6_Vw_vadd_VwVw(a0, a1);
   HVX_Vector sum1 = Q6_Vw_vadd_VwVw(a2, a3);
   HVX_Vector sum2 = Q6_Vw_vadd_VwVw(a4, a5);
   HVX_Vector sum3 = Q6_Vw_vadd_VwVw(a6, a7);
   HVX_Vector sum4 = Q6_Vw_vadd_VwVw(sum0, sum1);
   HVX_Vector sum5 = Q6_Vw_vadd_VwVw(sum2, sum3);
   HVX_Vector sum = Q6_Vw_vadd_VwVw(sum4, sum5);

   g_sink[0] = sum;
   return checksum_vector(sum) ^ checksum_vector(a0) ^ checksum_vector(a3) ^
          checksum_vector(a5) ^ checksum_vector(a7);
}

static uint64_t run_fp32_kernel(int iterations, uint64_t *checksum)
{
   HVX_Vector a0 = splat_u32(0x3f800000u);
   HVX_Vector a1 = splat_u32(0x3f810000u);
   HVX_Vector a2 = splat_u32(0x3f820000u);
   HVX_Vector a3 = splat_u32(0x3f830000u);
   HVX_Vector a4 = splat_u32(0x3f840000u);
   HVX_Vector a5 = splat_u32(0x3f850000u);
   HVX_Vector a6 = splat_u32(0x3f860000u);
   HVX_Vector a7 = splat_u32(0x3f870000u);
   const HVX_Vector b0 = splat_u32(0x3f000000u);
   const HVX_Vector b1 = splat_u32(0x3f010000u);
   const HVX_Vector b2 = splat_u32(0x3f020000u);
   const HVX_Vector b3 = splat_u32(0x3f030000u);
   const HVX_Vector b4 = splat_u32(0x3f040000u);
   const HVX_Vector b5 = splat_u32(0x3f050000u);
   const HVX_Vector b6 = splat_u32(0x3f060000u);
   const HVX_Vector b7 = splat_u32(0x3f070000u);

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      a0 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a0, b0), b0);
      a1 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a1, b1), b1);
      a2 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a2, b2), b2);
      a3 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a3, b3), b3);
      a4 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a4, b4), b4);
      a5 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a5, b5), b5);
      a6 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a6, b6), b6);
      a7 = Q6_Vsf_vadd_VsfVsf(Q6_Vsf_vmpy_VsfVsf(a7, b7), b7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(a0, a1, a2, a3, a4, a5, a6, a7);
   return elapsed_cycles(start, end);
}

static uint64_t run_fp16_kernel(int iterations, uint64_t *checksum)
{
   HVX_Vector a0 = splat_u32(0x3c003c00u);
   HVX_Vector a1 = splat_u32(0x3c103c10u);
   HVX_Vector a2 = splat_u32(0x3c203c20u);
   HVX_Vector a3 = splat_u32(0x3c303c30u);
   HVX_Vector a4 = splat_u32(0x3c403c40u);
   HVX_Vector a5 = splat_u32(0x3c503c50u);
   HVX_Vector a6 = splat_u32(0x3c603c60u);
   HVX_Vector a7 = splat_u32(0x3c703c70u);
   const HVX_Vector b0 = splat_u32(0x38003800u);
   const HVX_Vector b1 = splat_u32(0x38103810u);
   const HVX_Vector b2 = splat_u32(0x38203820u);
   const HVX_Vector b3 = splat_u32(0x38303830u);
   const HVX_Vector b4 = splat_u32(0x38403840u);
   const HVX_Vector b5 = splat_u32(0x38503850u);
   const HVX_Vector b6 = splat_u32(0x38603860u);
   const HVX_Vector b7 = splat_u32(0x38703870u);

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      a0 = Q6_Vhf_vmpyacc_VhfVhfVhf(a0, a0, b0);
      a1 = Q6_Vhf_vmpyacc_VhfVhfVhf(a1, a1, b1);
      a2 = Q6_Vhf_vmpyacc_VhfVhfVhf(a2, a2, b2);
      a3 = Q6_Vhf_vmpyacc_VhfVhfVhf(a3, a3, b3);
      a4 = Q6_Vhf_vmpyacc_VhfVhfVhf(a4, a4, b4);
      a5 = Q6_Vhf_vmpyacc_VhfVhfVhf(a5, a5, b5);
      a6 = Q6_Vhf_vmpyacc_VhfVhfVhf(a6, a6, b6);
      a7 = Q6_Vhf_vmpyacc_VhfVhfVhf(a7, a7, b7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(a0, a1, a2, a3, a4, a5, a6, a7);
   return elapsed_cycles(start, end);
}

static uint64_t run_qf16_kernel(int iterations, uint64_t *checksum)
{
   HVX_Vector a0 = splat_u32(0x3c003c00u);
   HVX_Vector a1 = splat_u32(0x3c103c10u);
   HVX_Vector a2 = splat_u32(0x3c203c20u);
   HVX_Vector a3 = splat_u32(0x3c303c30u);
   HVX_Vector a4 = splat_u32(0x3c403c40u);
   HVX_Vector a5 = splat_u32(0x3c503c50u);
   HVX_Vector a6 = splat_u32(0x3c603c60u);
   HVX_Vector a7 = splat_u32(0x3c703c70u);
   HVX_Vector b0 = splat_u32(0x10001000u);
   HVX_Vector b1 = splat_u32(0x10101010u);
   HVX_Vector b2 = splat_u32(0x10201020u);
   HVX_Vector b3 = splat_u32(0x10301030u);
   HVX_Vector b4 = splat_u32(0x10401040u);
   HVX_Vector b5 = splat_u32(0x10501050u);
   HVX_Vector b6 = splat_u32(0x10601060u);
   HVX_Vector b7 = splat_u32(0x10701070u);
   HVX_Vector acc0 = Q6_V_vzero();
   HVX_Vector acc1 = Q6_V_vzero();
   HVX_Vector acc2 = Q6_V_vzero();
   HVX_Vector acc3 = Q6_V_vzero();
   HVX_Vector acc4 = Q6_V_vzero();
   HVX_Vector acc5 = Q6_V_vzero();
   HVX_Vector acc6 = Q6_V_vzero();
   HVX_Vector acc7 = Q6_V_vzero();

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      KEEP_VECTORS4(a0, a1, a2, a3);
      KEEP_VECTORS4(a4, a5, a6, a7);
      KEEP_VECTORS4(b0, b1, b2, b3);
      KEEP_VECTORS4(b4, b5, b6, b7);

      HVX_Vector prod0 = Q6_Vqf16_vmpy_VhfVhf(a0, b0);
      HVX_Vector prod1 = Q6_Vqf16_vmpy_VhfVhf(a1, b1);
      HVX_Vector prod2 = Q6_Vqf16_vmpy_VhfVhf(a2, b2);
      HVX_Vector prod3 = Q6_Vqf16_vmpy_VhfVhf(a3, b3);
      HVX_Vector prod4 = Q6_Vqf16_vmpy_VhfVhf(a4, b4);
      HVX_Vector prod5 = Q6_Vqf16_vmpy_VhfVhf(a5, b5);
      HVX_Vector prod6 = Q6_Vqf16_vmpy_VhfVhf(a6, b6);
      HVX_Vector prod7 = Q6_Vqf16_vmpy_VhfVhf(a7, b7);

      acc0 = Q6_Vqf16_vadd_Vqf16Vqf16(acc0, prod0);
      acc1 = Q6_Vqf16_vadd_Vqf16Vqf16(acc1, prod1);
      acc2 = Q6_Vqf16_vadd_Vqf16Vqf16(acc2, prod2);
      acc3 = Q6_Vqf16_vadd_Vqf16Vqf16(acc3, prod3);
      acc4 = Q6_Vqf16_vadd_Vqf16Vqf16(acc4, prod4);
      acc5 = Q6_Vqf16_vadd_Vqf16Vqf16(acc5, prod5);
      acc6 = Q6_Vqf16_vadd_Vqf16Vqf16(acc6, prod6);
      acc7 = Q6_Vqf16_vadd_Vqf16Vqf16(acc7, prod7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(acc0, acc1, acc2, acc3, acc4, acc5,
                                     acc6, acc7);
   return elapsed_cycles(start, end);
}

static uint64_t run_qf32_kernel(int iterations, uint64_t *checksum)
{
   HVX_Vector a0 = splat_u32(0x3f800000u);
   HVX_Vector a1 = splat_u32(0x3f810000u);
   HVX_Vector a2 = splat_u32(0x3f820000u);
   HVX_Vector a3 = splat_u32(0x3f830000u);
   HVX_Vector a4 = splat_u32(0x3f840000u);
   HVX_Vector a5 = splat_u32(0x3f850000u);
   HVX_Vector a6 = splat_u32(0x3f860000u);
   HVX_Vector a7 = splat_u32(0x3f870000u);
   HVX_Vector b0 = splat_u32(0x3f000000u);
   HVX_Vector b1 = splat_u32(0x3f010000u);
   HVX_Vector b2 = splat_u32(0x3f020000u);
   HVX_Vector b3 = splat_u32(0x3f030000u);
   HVX_Vector b4 = splat_u32(0x3f040000u);
   HVX_Vector b5 = splat_u32(0x3f050000u);
   HVX_Vector b6 = splat_u32(0x3f060000u);
   HVX_Vector b7 = splat_u32(0x3f070000u);
   HVX_Vector acc0 = Q6_V_vzero();
   HVX_Vector acc1 = Q6_V_vzero();
   HVX_Vector acc2 = Q6_V_vzero();
   HVX_Vector acc3 = Q6_V_vzero();
   HVX_Vector acc4 = Q6_V_vzero();
   HVX_Vector acc5 = Q6_V_vzero();
   HVX_Vector acc6 = Q6_V_vzero();
   HVX_Vector acc7 = Q6_V_vzero();

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      KEEP_VECTORS4(a0, a1, a2, a3);
      KEEP_VECTORS4(a4, a5, a6, a7);
      KEEP_VECTORS4(b0, b1, b2, b3);
      KEEP_VECTORS4(b4, b5, b6, b7);

      HVX_Vector prod0 = Q6_Vqf32_vmpy_VsfVsf(a0, b0);
      HVX_Vector prod1 = Q6_Vqf32_vmpy_VsfVsf(a1, b1);
      HVX_Vector prod2 = Q6_Vqf32_vmpy_VsfVsf(a2, b2);
      HVX_Vector prod3 = Q6_Vqf32_vmpy_VsfVsf(a3, b3);
      HVX_Vector prod4 = Q6_Vqf32_vmpy_VsfVsf(a4, b4);
      HVX_Vector prod5 = Q6_Vqf32_vmpy_VsfVsf(a5, b5);
      HVX_Vector prod6 = Q6_Vqf32_vmpy_VsfVsf(a6, b6);
      HVX_Vector prod7 = Q6_Vqf32_vmpy_VsfVsf(a7, b7);

      acc0 = Q6_Vqf32_vadd_Vqf32Vqf32(acc0, prod0);
      acc1 = Q6_Vqf32_vadd_Vqf32Vqf32(acc1, prod1);
      acc2 = Q6_Vqf32_vadd_Vqf32Vqf32(acc2, prod2);
      acc3 = Q6_Vqf32_vadd_Vqf32Vqf32(acc3, prod3);
      acc4 = Q6_Vqf32_vadd_Vqf32Vqf32(acc4, prod4);
      acc5 = Q6_Vqf32_vadd_Vqf32Vqf32(acc5, prod5);
      acc6 = Q6_Vqf32_vadd_Vqf32Vqf32(acc6, prod6);
      acc7 = Q6_Vqf32_vadd_Vqf32Vqf32(acc7, prod7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(acc0, acc1, acc2, acc3, acc4, acc5,
                                     acc6, acc7);
   return elapsed_cycles(start, end);
}

static uint64_t run_int8_fixed_kernel(int iterations, uint64_t *checksum)
{
   const HVX_Vector a0 = splat_u32(0x01020304u);
   const HVX_Vector a1 = splat_u32(0x05060708u);
   const HVX_Vector a2 = splat_u32(0x090a0b0cu);
   const HVX_Vector a3 = splat_u32(0x0d0e0f10u);
   const HVX_Vector a4 = splat_u32(0x11121314u);
   const HVX_Vector a5 = splat_u32(0x15161718u);
   const HVX_Vector a6 = splat_u32(0x191a1b1cu);
   const HVX_Vector a7 = splat_u32(0x1d1e1f20u);
   HVX_Vector acc0 = Q6_V_vzero();
   HVX_Vector acc1 = Q6_V_vzero();
   HVX_Vector acc2 = Q6_V_vzero();
   HVX_Vector acc3 = Q6_V_vzero();
   HVX_Vector acc4 = Q6_V_vzero();
   HVX_Vector acc5 = Q6_V_vzero();
   HVX_Vector acc6 = Q6_V_vzero();
   HVX_Vector acc7 = Q6_V_vzero();
   const HVX_Vector b0 = splat_u32(0x01010101u);
   const HVX_Vector b1 = splat_u32(0x02020202u);
   const HVX_Vector b2 = splat_u32(0x03030303u);
   const HVX_Vector b3 = splat_u32(0x04040404u);
   const HVX_Vector b4 = splat_u32(0x05050505u);
   const HVX_Vector b5 = splat_u32(0x06060606u);
   const HVX_Vector b6 = splat_u32(0x07070707u);
   const HVX_Vector b7 = splat_u32(0x08080808u);

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      acc0 = Q6_Vw_vrmpyacc_VwVubVb(acc0, a0, b0);
      acc1 = Q6_Vw_vrmpyacc_VwVubVb(acc1, a1, b1);
      acc2 = Q6_Vw_vrmpyacc_VwVubVb(acc2, a2, b2);
      acc3 = Q6_Vw_vrmpyacc_VwVubVb(acc3, a3, b3);
      acc4 = Q6_Vw_vrmpyacc_VwVubVb(acc4, a4, b4);
      acc5 = Q6_Vw_vrmpyacc_VwVubVb(acc5, a5, b5);
      acc6 = Q6_Vw_vrmpyacc_VwVubVb(acc6, a6, b6);
      acc7 = Q6_Vw_vrmpyacc_VwVubVb(acc7, a7, b7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(acc0, acc1, acc2, acc3, acc4, acc5,
                                     acc6, acc7);
   return elapsed_cycles(start, end);
}

static uint64_t run_int8_add_fixed_kernel(int iterations, uint64_t *checksum)
{
   HVX_Vector a0 = splat_u32(0x01020304u);
   HVX_Vector a1 = splat_u32(0x05060708u);
   HVX_Vector a2 = splat_u32(0x090a0b0cu);
   HVX_Vector a3 = splat_u32(0x0d0e0f10u);
   HVX_Vector a4 = splat_u32(0x11121314u);
   HVX_Vector a5 = splat_u32(0x15161718u);
   HVX_Vector a6 = splat_u32(0x191a1b1cu);
   HVX_Vector a7 = splat_u32(0x1d1e1f20u);
   HVX_Vector acc0 = Q6_V_vzero();
   HVX_Vector acc1 = Q6_V_vzero();
   HVX_Vector acc2 = Q6_V_vzero();
   HVX_Vector acc3 = Q6_V_vzero();
   HVX_Vector acc4 = Q6_V_vzero();
   HVX_Vector acc5 = Q6_V_vzero();
   HVX_Vector acc6 = Q6_V_vzero();
   HVX_Vector acc7 = Q6_V_vzero();
   HVX_Vector b0 = splat_u32(0x01010101u);
   HVX_Vector b1 = splat_u32(0x02020202u);
   HVX_Vector b2 = splat_u32(0x03030303u);
   HVX_Vector b3 = splat_u32(0x04040404u);
   HVX_Vector b4 = splat_u32(0x05050505u);
   HVX_Vector b5 = splat_u32(0x06060606u);
   HVX_Vector b6 = splat_u32(0x07070707u);
   HVX_Vector b7 = splat_u32(0x08080808u);

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int i = 0; i < iterations; ++i) {
      KEEP_VECTORS4(a0, a1, a2, a3);
      KEEP_VECTORS4(a4, a5, a6, a7);
      KEEP_VECTORS4(b0, b1, b2, b3);
      KEEP_VECTORS4(b4, b5, b6, b7);

      HVX_Vector prod0 = Q6_Vw_vrmpy_VubVb(a0, b0);
      HVX_Vector prod1 = Q6_Vw_vrmpy_VubVb(a1, b1);
      HVX_Vector prod2 = Q6_Vw_vrmpy_VubVb(a2, b2);
      HVX_Vector prod3 = Q6_Vw_vrmpy_VubVb(a3, b3);
      HVX_Vector prod4 = Q6_Vw_vrmpy_VubVb(a4, b4);
      HVX_Vector prod5 = Q6_Vw_vrmpy_VubVb(a5, b5);
      HVX_Vector prod6 = Q6_Vw_vrmpy_VubVb(a6, b6);
      HVX_Vector prod7 = Q6_Vw_vrmpy_VubVb(a7, b7);

      acc0 = Q6_Vw_vadd_VwVw(acc0, prod0);
      acc1 = Q6_Vw_vadd_VwVw(acc1, prod1);
      acc2 = Q6_Vw_vadd_VwVw(acc2, prod2);
      acc3 = Q6_Vw_vadd_VwVw(acc3, prod3);
      acc4 = Q6_Vw_vadd_VwVw(acc4, prod4);
      acc5 = Q6_Vw_vadd_VwVw(acc5, prod5);
      acc6 = Q6_Vw_vadd_VwVw(acc6, prod6);
      acc7 = Q6_Vw_vadd_VwVw(acc7, prod7);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *checksum = checksum_accumulators(acc0, acc1, acc2, acc3, acc4, acc5,
                                     acc6, acc7);
   return elapsed_cycles(start, end);
}

static uint64_t checksum_bytes(const uint8_t *data, int len)
{
   uint64_t sum = 0;

   if (len <= 0)
      return 0;

   for (int i = 0; i < len; i += 4096)
      sum += data[i];
   sum += data[len - 1];
   return sum;
}

int cdsp_peak_open(const char *uri, remote_handle64 *handle)
{
   (void)uri;

   g_state.timer_overhead = measure_timer_overhead();
   *handle = (remote_handle64)(uintptr_t)&g_state;
   return AEE_SUCCESS;
}

int cdsp_peak_close(remote_handle64 handle)
{
   (void)handle;
   return AEE_SUCCESS;
}

int cdsp_peak_get_info(remote_handle64 handle, int *arch, int *hvx_bytes,
                       uint64_t *timer_overhead, uint64_t *current_cycles)
{
   (void)handle;

   *arch = CDSP_PEAK_ARCH;
   *hvx_bytes = (int)sizeof(HVX_Vector);
   *timer_overhead = g_state.timer_overhead;
   *current_cycles = read_cycles();
   return AEE_SUCCESS;
}

int cdsp_peak_bench_compute(remote_handle64 handle, int kernel, int iterations,
                            uint64_t *cycles, uint64_t *checksum)
{
   (void)handle;

   if (iterations <= 0)
      return AEE_EBADPARM;

   switch (kernel) {
   case CDSP_PEAK_KERNEL_FP32:
      *cycles = run_fp32_kernel(iterations, checksum);
      break;
   case CDSP_PEAK_KERNEL_FP16:
      *cycles = run_fp16_kernel(iterations, checksum);
      break;
   case CDSP_PEAK_KERNEL_INT8_FIXED:
      *cycles = run_int8_fixed_kernel(iterations, checksum);
      break;
   case CDSP_PEAK_KERNEL_INT8_ADD_FIXED:
      *cycles = run_int8_add_fixed_kernel(iterations, checksum);
      break;
   case CDSP_PEAK_KERNEL_QF16:
      *cycles = run_qf16_kernel(iterations, checksum);
      break;
   case CDSP_PEAK_KERNEL_QF32:
      *cycles = run_qf32_kernel(iterations, checksum);
      break;
   default:
      return AEE_EBADPARM;
   }

   return AEE_SUCCESS;
}

int cdsp_peak_bench_mem_read(remote_handle64 handle, const uint8_t *src,
                             int srcLen, int repeats, uint64_t *cycles,
                             uint64_t *checksum)
{
   const int nvec = srcLen / (int)sizeof(HVX_Vector);
   const HVX_Vector *vectors = (const HVX_Vector *)src;
   HVX_Vector acc = Q6_V_vzero();

   (void)handle;
   if (!src || srcLen <= 0 || repeats <= 0 || nvec <= 0)
      return AEE_EBADPARM;

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int r = 0; r < repeats; ++r) {
      for (int i = 0; i < nvec; ++i)
         acc = Q6_Vw_vadd_VwVw(acc, vectors[i]);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   g_sink[0] = acc;
   *cycles = elapsed_cycles(start, end);
   *checksum = checksum_vector(acc);
   return AEE_SUCCESS;
}

int cdsp_peak_bench_mem_write(remote_handle64 handle, uint8_t *dst, int dstLen,
                              int repeats, uint64_t *cycles,
                              uint64_t *checksum)
{
   const int nvec = dstLen / (int)sizeof(HVX_Vector);
   HVX_Vector *vectors = (HVX_Vector *)dst;
   HVX_Vector value = splat_u32(0x5a5a5a5au);
   const HVX_Vector step = splat_u32(0x01010101u);

   (void)handle;
   if (!dst || dstLen <= 0 || repeats <= 0 || nvec <= 0)
      return AEE_EBADPARM;

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int r = 0; r < repeats; ++r) {
      for (int i = 0; i < nvec; ++i)
         vectors[i] = value;
      value = Q6_Vw_vadd_VwVw(value, step);
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   *cycles = elapsed_cycles(start, end);
   *checksum = checksum_bytes(dst, dstLen);
   return AEE_SUCCESS;
}

int cdsp_peak_bench_mem_copy(remote_handle64 handle, const uint8_t *src,
                             int srcLen, uint8_t *dst, int dstLen,
                             int repeats, uint64_t *cycles,
                             uint64_t *checksum)
{
   int nvec;
   const HVX_Vector *src_vectors = (const HVX_Vector *)src;
   HVX_Vector *dst_vectors = (HVX_Vector *)dst;
   HVX_Vector acc = Q6_V_vzero();

   (void)handle;
   if (!src || !dst || srcLen <= 0 || dstLen <= 0 || repeats <= 0)
      return AEE_EBADPARM;

   nvec = srcLen < dstLen ? srcLen : dstLen;
   nvec /= (int)sizeof(HVX_Vector);
   if (nvec <= 0)
      return AEE_EBADPARM;

   __asm__ volatile("" ::: "memory");
   uint64_t start = read_cycles();
   for (int r = 0; r < repeats; ++r) {
      for (int i = 0; i < nvec; ++i) {
         HVX_Vector value = src_vectors[i];
         dst_vectors[i] = value;
         acc = Q6_Vw_vadd_VwVw(acc, value);
      }
   }
   uint64_t end = read_cycles();
   __asm__ volatile("" ::: "memory");

   g_sink[0] = acc;
   *cycles = elapsed_cycles(start, end);
   *checksum = checksum_vector(acc) + checksum_bytes(dst, nvec * (int)sizeof(HVX_Vector));
   return AEE_SUCCESS;
}
