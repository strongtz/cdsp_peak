// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <hexagon_types.h>
#include <hmx_hexagon_protos.h>

#include "AEEStdErr.h"
#include "cdsp_peak.h"

#define HMX_TILE_U8_BYTES 1024
#define HMX_TILE_U16_BYTES 2048
#define HMX_TILE_U16_2X2_BYTES 8192
#define HMX_DEEP_TILES 32
#define HMX_BIAS_BYTES 256

#define CDSP_PEAK_HMX_INT8_UB 0
#define CDSP_PEAK_HMX_INT8_CM_UB 1
#define CDSP_PEAK_HMX_INT8_UH 2
#define CDSP_PEAK_HMX_INT8_UB_X16 10
#define CDSP_PEAK_HMX_INT8_UH_X16 11
#define CDSP_PEAK_HMX_INT8_UB_FULL_X16 12
#define CDSP_PEAK_HMX_INT8_UB_FULL_X32 13
#define CDSP_PEAK_HMX_INT8_UB_FULL_X64 14
#define CDSP_PEAK_HMX_INT8_UH2X2_FULL_X64 15
#define CDSP_PEAK_HMX_INT8_UB_ADEEP32 16
#define CDSP_PEAK_HMX_INT8_UB_WDEEP_FULL_X64 17
#define CDSP_PEAK_HMX_FP16_HF_X64 18
#define CDSP_PEAK_HMX_INT4_UB_WN2X_FULL_X64 19
#define CDSP_PEAK_HMX_PROBE_RESOURCE 100
#define CDSP_PEAK_HMX_PROBE_CACHED 101
#define CDSP_PEAK_HMX_PROBE_LOCK 102

#define CDSP_PEAK_HMX_ERR_BASE 0x7000
#define CDSP_PEAK_HMX_ERR(step, err) \
   (CDSP_PEAK_HMX_ERR_BASE + ((step) << 8) + ((err) & 0xff))

typedef struct __attribute__((aligned(8))) {
   unsigned long long attributes[20];
} compute_res_attr_t;

extern int compute_resource_query_VTCM(unsigned int request,
                                       unsigned int *vtcm_size,
                                       void *unused0, void *unused1,
                                       void *unused2) __attribute__((weak));
extern int compute_resource_attr_init(compute_res_attr_t *attr)
   __attribute__((weak));
extern int compute_resource_attr_set_cache_mode(compute_res_attr_t *attr,
                                                int cache_mode)
   __attribute__((weak));
extern int compute_resource_attr_set_vtcm_param_v2(compute_res_attr_t *attr,
                                                   unsigned int vtcm_size,
                                                   unsigned int page_size,
                                                   unsigned int min_size)
   __attribute__((weak));
extern int compute_resource_attr_set_hmx_param(compute_res_attr_t *attr,
                                               int enable_hmx)
   __attribute__((weak));
extern unsigned int compute_resource_acquire(compute_res_attr_t *attr,
                                             unsigned int timeout_us)
   __attribute__((weak));
extern int compute_resource_acquire_cached(unsigned int rctx,
                                           unsigned int timeout_us)
   __attribute__((weak));
extern int compute_resource_attr_get_vtcm_ptr_v2(compute_res_attr_t *attr,
                                                 void **vtcm_ptr,
                                                 unsigned int *vtcm_size)
   __attribute__((weak));
extern int compute_resource_release(unsigned int rctx) __attribute__((weak));
extern int compute_resource_release_cached(unsigned int rctx)
   __attribute__((weak));
extern int compute_resource_hmx_lock(unsigned int rctx) __attribute__((weak));
extern int compute_resource_hmx_unlock(unsigned int rctx) __attribute__((weak));

struct hmx_resource {
   unsigned int rctx;
   uint8_t *vtcm;
   unsigned int vtcm_size;
   int cached;
   int locked;
};

static void hmx_resource_release(struct hmx_resource *res);

static inline uint64_t read_cycles_hmx(void)
{
   uint64_t cycles;
   __asm__ volatile("%0 = c15:14" : "=r"(cycles));
   return cycles;
}

static uint64_t measure_timer_overhead_hmx(void)
{
   uint64_t best = UINT64_MAX;

   for (int i = 0; i < 128; ++i) {
      uint64_t start = read_cycles_hmx();
      uint64_t end = read_cycles_hmx();
      uint64_t delta = end - start;

      if (delta < best)
         best = delta;
   }

   return best == UINT64_MAX ? 0 : best;
}

static inline uint64_t elapsed_cycles_hmx(uint64_t start, uint64_t end,
                                          uint64_t overhead)
{
   uint64_t delta = end - start;

   return delta > overhead ? delta - overhead : delta;
}

static uint64_t checksum_hmx_output(const uint8_t *data, int len)
{
   uint64_t sum = 0xcbf29ce484222325ULL;

   for (int i = 0; i < len; ++i) {
      sum ^= (uint64_t)data[i] + ((uint64_t)(uint32_t)i << 32);
      sum *= 0x100000001b3ULL;
   }

   return sum;
}

static inline uint32_t hmx_addr(const void *ptr)
{
   return (uint32_t)(uintptr_t)ptr;
}

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t alignment)
{
   return (value + alignment - 1u) & ~(alignment - 1u);
}

static int has_compute_resource_api(void)
{
   return compute_resource_attr_init &&
          compute_resource_attr_set_cache_mode &&
          compute_resource_attr_set_vtcm_param_v2 &&
          compute_resource_attr_set_hmx_param &&
          compute_resource_acquire &&
          compute_resource_attr_get_vtcm_ptr_v2 &&
          compute_resource_release &&
          compute_resource_acquire_cached &&
          compute_resource_release_cached &&
          compute_resource_hmx_lock &&
          compute_resource_hmx_unlock;
}

static int hmx_resource_acquire(struct hmx_resource *res,
                                unsigned int required_bytes,
                                int use_cached, int use_lock)
{
   compute_res_attr_t attr;
   unsigned int vtcm_available = 0;
   unsigned int vtcm_size = (required_bytes + 4095u) & ~4095u;
   void *vtcm_ptr = NULL;
   int err;

   memset(res, 0, sizeof(*res));
   if (!has_compute_resource_api())
      return AEE_EUNSUPPORTED;

   if (compute_resource_query_VTCM &&
       !compute_resource_query_VTCM(0, &vtcm_available, NULL, NULL, NULL) &&
       vtcm_available < required_bytes)
      return CDSP_PEAK_HMX_ERR(0, AEE_ENOMEMORY);

   memset(&attr, 0, sizeof(attr));
   err = compute_resource_attr_init(&attr);
   if (err)
      return CDSP_PEAK_HMX_ERR(1, err);

   err = compute_resource_attr_set_cache_mode(&attr, 1);
   if (err)
      return CDSP_PEAK_HMX_ERR(2, err);

   err = compute_resource_attr_set_vtcm_param_v2(&attr, vtcm_size, vtcm_size,
                                                 vtcm_size);
   if (err)
      return CDSP_PEAK_HMX_ERR(3, err);

   err = compute_resource_attr_set_hmx_param(&attr, 1);
   if (err)
      return CDSP_PEAK_HMX_ERR(4, err);

   res->rctx = compute_resource_acquire(&attr, 1000000u);
   if (!res->rctx)
      return CDSP_PEAK_HMX_ERR(5, AEE_ENOMEMORY);

   if (use_cached) {
      err = compute_resource_acquire_cached(res->rctx, 1000000u);
      if (err) {
         compute_resource_release(res->rctx);
         memset(res, 0, sizeof(*res));
         return CDSP_PEAK_HMX_ERR(6, err);
      }
      res->cached = 1;
   }

   err = compute_resource_attr_get_vtcm_ptr_v2(&attr, &vtcm_ptr, &vtcm_size);
   if (err || !vtcm_ptr) {
      if (res->cached)
         compute_resource_release_cached(res->rctx);
      compute_resource_release(res->rctx);
      memset(res, 0, sizeof(*res));
      return CDSP_PEAK_HMX_ERR(7, err ? err : AEE_ENOMEMORY);
   }

   res->vtcm = (uint8_t *)vtcm_ptr;
   res->vtcm_size = vtcm_size;
   if (use_lock) {
      err = compute_resource_hmx_lock(res->rctx);
      if (err) {
         hmx_resource_release(res);
         return CDSP_PEAK_HMX_ERR(8, err);
      }
      res->locked = 1;
   }

   return AEE_SUCCESS;
}

static void hmx_resource_release(struct hmx_resource *res)
{
   if (res->locked)
      (void)compute_resource_hmx_unlock(res->rctx);
   if (res->cached)
      (void)compute_resource_release_cached(res->rctx);
   if (res->rctx)
      (void)compute_resource_release(res->rctx);
   memset(res, 0, sizeof(*res));
}

static __attribute__((noinline)) void hmx_int8_tile_ub(const uint8_t *activation,
                                                       const uint8_t *weight,
                                                       uint8_t *output)
{
   const uint32_t limit = HMX_TILE_U8_BYTES - 1;

   Q6_mxclracc();
   Q6_activation_ub_mxmem_RR(hmx_addr(activation), limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight), limit);
   Q6_mxmem_AR_after_sat_ub(output, 0);
}

static __attribute__((noinline)) void hmx_int8_tile_cm_ub(const uint8_t *activation,
                                                          const uint8_t *weight,
                                                          const uint8_t *weight2,
                                                          const uint8_t *bias,
                                                          uint8_t *output)
{
   const uint32_t activation_limit = 0x1c;
   const uint32_t weight_limit = HMX_TILE_U8_BYTES - 1;

   Q6_mxclracc();
   Q6_bias_mxmem2_A((void *)bias);
   Q6_activation_ub_mxmem_RR_cm(hmx_addr(activation), activation_limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight), weight_limit);
   Q6_activation_ub_mxmem_RR_cm(hmx_addr(activation), activation_limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight2), weight_limit);
   Q6_mxmem_AR_after_cm_sat_ub(output, 0);
}

static __attribute__((noinline)) void hmx_int8_tile_uh(const uint8_t *activation,
                                                       const uint8_t *weight,
                                                       uint8_t *output)
{
   const uint32_t limit = HMX_TILE_U8_BYTES - 1;

   Q6_mxclracc();
   Q6_activation_ub_mxmem_RR(hmx_addr(activation), limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight), limit);
   Q6_mxmem_AR_after_sat_uh_2x1(output, 0);
}

static __attribute__((always_inline)) inline void
hmx_int8_accumulate_ub(const uint8_t *activation, const uint8_t *weight)
{
   const uint32_t limit = HMX_TILE_U8_BYTES - 1;

   Q6_activation_ub_mxmem_RR(hmx_addr(activation), limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight), limit);
}

static __attribute__((always_inline)) inline void
hmx_int8_accumulate_ub_wdeep(const uint8_t *activation, const uint8_t *weight)
{
   const uint32_t activation_limit = HMX_TILE_U8_BYTES - 1;
   const uint32_t weight_limit = (2u * HMX_TILE_U8_BYTES) - 1u;

   Q6_activation_ub_mxmem_RR(hmx_addr(activation), activation_limit);
   Q6_weight_b_mxmem_RR_deep(hmx_addr(weight), weight_limit);
}

static __attribute__((always_inline)) inline void
hmx_int8_accumulate_ub_x16(const uint8_t *activation, const uint8_t *weight)
{
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
   hmx_int8_accumulate_ub(activation, weight);
}

static __attribute__((always_inline)) inline void
hmx_int8_accumulate_ub_wdeep_x16(const uint8_t *activation,
                                 const uint8_t *weight)
{
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
   hmx_int8_accumulate_ub_wdeep(activation, weight);
}

static void hmx_init_fp16_scales(uint8_t *bias)
{
   memset(bias, 0, HMX_BIAS_BYTES);
   for (int i = 0; i < 128; i += 4) {
      bias[i] = 0x00;
      bias[i + 1] = 0x3c;
   }
}

static __attribute__((always_inline)) inline void
hmx_fp16_accumulate_hf(const uint8_t *activation, const uint8_t *weight)
{
   const uint32_t limit = HMX_TILE_U16_BYTES - 1u;

   Q6_activation_hf_mxmem_RR(hmx_addr(activation), limit);
   Q6_weight_hf_mxmem_RR(hmx_addr(weight), limit);
}

static __attribute__((always_inline)) inline void
hmx_fp16_accumulate_hf_x16(const uint8_t *activation, const uint8_t *weight)
{
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
   hmx_fp16_accumulate_hf(activation, weight);
}

static __attribute__((noinline)) void
hmx_fp16_tile_hf_x64(const uint8_t *activation, const uint8_t *weight,
                     uint8_t *bias, uint8_t *output)
{
   Q6_bias_mxmem2_A((void *)bias);
   Q6_mxclracc_hf();
   hmx_fp16_accumulate_hf_x16(activation, weight);
   hmx_fp16_accumulate_hf_x16(activation, weight);
   hmx_fp16_accumulate_hf_x16(activation, weight);
   hmx_fp16_accumulate_hf_x16(activation, weight);
   Q6_mxmem_AR_after_hf(output, 0);
}

#if __HMX_ARCH__ >= 73
static __attribute__((always_inline)) inline void
hmx_int4_accumulate_ub_wn2x(const uint8_t *activation, const uint8_t *weight)
{
   const uint32_t activation_limit = HMX_TILE_U8_BYTES - 1u;
   const uint32_t weight_limit = HMX_TILE_U8_BYTES - 1u;

   Q6_activation_ub_mxmem_RR(hmx_addr(activation), activation_limit);
   Q6_weight_n_mxmem_RR_2x(hmx_addr(weight), weight_limit);
}

static __attribute__((always_inline)) inline void
hmx_int4_accumulate_ub_wn2x_x16(const uint8_t *activation,
                                const uint8_t *weight)
{
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
   hmx_int4_accumulate_ub_wn2x(activation, weight);
}
#endif

static __attribute__((noinline)) int
hmx_int4_tile_ub_wn2x_full_x64(const uint8_t *activation,
                               const uint8_t *weight, uint8_t *output)
{
#if __HMX_ARCH__ >= 73
   Q6_mxclracc();
   hmx_int4_accumulate_ub_wn2x_x16(activation, weight);
   hmx_int4_accumulate_ub_wn2x_x16(activation, weight);
   hmx_int4_accumulate_ub_wn2x_x16(activation, weight);
   hmx_int4_accumulate_ub_wn2x_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_ub(output, 0);
   Q6_mxmem_AR_after_sat_ub(output + HMX_TILE_U8_BYTES, 0);
   return AEE_SUCCESS;
#else
   (void)activation;
   (void)weight;
   (void)output;
   return AEE_EUNSUPPORTED;
#endif
}

static __attribute__((noinline)) void
hmx_int8_tile_ub_adeep32(const uint8_t *activation, const uint8_t *weight,
                         uint8_t *output)
{
   const uint32_t limit = (HMX_DEEP_TILES * HMX_TILE_U8_BYTES) - 1u;

   Q6_mxclracc();
   Q6_activation_ub_mxmem_RR_deep(hmx_addr(activation), limit);
   Q6_weight_b_mxmem_RR(hmx_addr(weight), limit);
   Q6_mxmem_AR_after_sat_ub(output, 0);
}

static __attribute__((noinline)) void hmx_int8_tile_ub_x16(const uint8_t *activation,
                                                           const uint8_t *weight,
                                                           uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_after_sat_ub(output, 0);
}

static __attribute__((noinline)) void
hmx_int8_tile_ub_full_x16(const uint8_t *activation, const uint8_t *weight,
                          uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_ub(output, 0);
   Q6_mxmem_AR_after_sat_ub(output + HMX_TILE_U8_BYTES, 0);
}

static __attribute__((noinline)) void
hmx_int8_tile_ub_full_x32(const uint8_t *activation, const uint8_t *weight,
                          uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_ub(output, 0);
   Q6_mxmem_AR_after_sat_ub(output + HMX_TILE_U8_BYTES, 0);
}

static __attribute__((noinline)) void
hmx_int8_tile_ub_full_x64(const uint8_t *activation, const uint8_t *weight,
                          uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_ub(output, 0);
   Q6_mxmem_AR_after_sat_ub(output + HMX_TILE_U8_BYTES, 0);
}

static __attribute__((noinline)) void
hmx_int8_tile_ub_wdeep_full_x64(const uint8_t *activation,
                                const uint8_t *weight, uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_wdeep_x16(activation, weight);
   hmx_int8_accumulate_ub_wdeep_x16(activation, weight);
   hmx_int8_accumulate_ub_wdeep_x16(activation, weight);
   hmx_int8_accumulate_ub_wdeep_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_ub(output, 0);
   Q6_mxmem_AR_after_sat_ub(output + HMX_TILE_U8_BYTES, 0);
}

static __attribute__((noinline)) int
hmx_int8_tile_uh2x2_full_x64(const uint8_t *activation, const uint8_t *weight,
                             uint8_t *output)
{
#if __HMX_ARCH__ >= 69
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_before_retain_sat_uh_2x2(output, 0);
   Q6_mxmem_AR_after_sat_uh_2x2(output + (HMX_TILE_U16_2X2_BYTES / 2), 0);
   return AEE_SUCCESS;
#else
   (void)activation;
   (void)weight;
   (void)output;
   return AEE_EUNSUPPORTED;
#endif
}

static __attribute__((noinline)) void hmx_int8_tile_uh_x16(const uint8_t *activation,
                                                           const uint8_t *weight,
                                                           uint8_t *output)
{
   Q6_mxclracc();
   hmx_int8_accumulate_ub_x16(activation, weight);
   Q6_mxmem_AR_after_sat_uh_2x1(output, 0);
}

int cdsp_peak_bench_hmx_int8(remote_handle64 handle,
                             const uint8_t *activation, int activationLen,
                             const uint8_t *weight, int weightLen,
                             uint8_t *output, int outputLen,
                             int mode, int repeats,
                             uint64_t *cycles, uint64_t *checksum)
{
   int written_len;
   int is_probe = 0;
   int use_cached = 1;
   int use_lock = 1;
   struct hmx_resource resource;
   uint8_t *vtcm_activation;
   uint8_t *vtcm_weight;
   uint8_t *vtcm_weight2;
   uint8_t *vtcm_bias;
   uint8_t *vtcm_output;
   unsigned int activation_vtcm_len = HMX_TILE_U8_BYTES;
   unsigned int weight_vtcm_len = HMX_TILE_U8_BYTES;
   unsigned int required_vtcm;
   uintptr_t aligned_base;
   int err;
   uint64_t overhead;
   uint64_t start;
   uint64_t end;

   (void)handle;
   if (!activation || !weight || !output || !cycles || !checksum ||
       repeats <= 0)
      return AEE_EBADPARM;

   switch (mode) {
   case CDSP_PEAK_HMX_INT8_UB:
   case CDSP_PEAK_HMX_INT8_CM_UB:
   case CDSP_PEAK_HMX_INT8_UB_X16:
      written_len = HMX_TILE_U8_BYTES;
      break;
   case CDSP_PEAK_HMX_INT8_UB_ADEEP32:
      activation_vtcm_len = HMX_DEEP_TILES * HMX_TILE_U8_BYTES;
      weight_vtcm_len = HMX_DEEP_TILES * HMX_TILE_U8_BYTES;
      written_len = HMX_TILE_U8_BYTES;
      break;
   case CDSP_PEAK_HMX_INT8_UB_FULL_X16:
   case CDSP_PEAK_HMX_INT8_UB_FULL_X32:
   case CDSP_PEAK_HMX_INT8_UB_FULL_X64:
   case CDSP_PEAK_HMX_INT8_UB_WDEEP_FULL_X64:
   case CDSP_PEAK_HMX_INT4_UB_WN2X_FULL_X64:
   case CDSP_PEAK_HMX_FP16_HF_X64:
      written_len = HMX_TILE_U16_BYTES;
      break;
   case CDSP_PEAK_HMX_INT8_UH2X2_FULL_X64:
      written_len = HMX_TILE_U16_2X2_BYTES;
      break;
   case CDSP_PEAK_HMX_INT8_UH:
   case CDSP_PEAK_HMX_INT8_UH_X16:
      written_len = HMX_TILE_U16_BYTES;
      break;
   case CDSP_PEAK_HMX_PROBE_RESOURCE:
      written_len = 16;
      is_probe = 1;
      use_cached = 0;
      use_lock = 0;
      break;
   case CDSP_PEAK_HMX_PROBE_CACHED:
      written_len = 16;
      is_probe = 1;
      use_lock = 0;
      break;
   case CDSP_PEAK_HMX_PROBE_LOCK:
      written_len = 16;
      is_probe = 1;
      break;
   default:
      return AEE_EBADPARM;
   }

   if (mode == CDSP_PEAK_HMX_INT8_UB_WDEEP_FULL_X64)
      weight_vtcm_len = 2u * HMX_TILE_U8_BYTES;
   else if (mode == CDSP_PEAK_HMX_FP16_HF_X64) {
      activation_vtcm_len = HMX_TILE_U16_BYTES;
      weight_vtcm_len = HMX_TILE_U16_BYTES;
   }

   if (activationLen < (int)activation_vtcm_len ||
       weightLen < (int)weight_vtcm_len)
      return AEE_EBADPARM;

   if (outputLen < written_len)
      return AEE_EBADPARM;

   required_vtcm = activation_vtcm_len + weight_vtcm_len +
                   HMX_TILE_U8_BYTES + HMX_BIAS_BYTES +
                   (unsigned int)written_len + 128u;

   err = hmx_resource_acquire(&resource, required_vtcm, use_cached, use_lock);
   if (err)
      return err;

   if (is_probe) {
      uint32_t probe_words[4] = {
         resource.rctx,
         resource.vtcm_size,
         (uint32_t)resource.cached,
         (uint32_t)resource.locked,
      };

      memset(output, 0, (size_t)written_len);
      memcpy(output, probe_words, sizeof(probe_words));
      *cycles = 0;
      *checksum = checksum_hmx_output(output, written_len);
      hmx_resource_release(&resource);
      return AEE_SUCCESS;
   }

   aligned_base = align_up_uintptr((uintptr_t)resource.vtcm, 128u);
   if (aligned_base + required_vtcm >
       (uintptr_t)resource.vtcm + (uintptr_t)resource.vtcm_size) {
      hmx_resource_release(&resource);
      return AEE_ENOMEMORY;
   }

   vtcm_activation = (uint8_t *)aligned_base;
   vtcm_weight = vtcm_activation + activation_vtcm_len;
   vtcm_weight2 = vtcm_weight + weight_vtcm_len;
   vtcm_bias = vtcm_weight2 + HMX_TILE_U8_BYTES;
   vtcm_output = vtcm_bias + HMX_BIAS_BYTES;

   memcpy(vtcm_activation, activation, activation_vtcm_len);
   memcpy(vtcm_weight, weight, weight_vtcm_len);
   memcpy(vtcm_weight2, weight, HMX_TILE_U8_BYTES);
   if (mode == CDSP_PEAK_HMX_FP16_HF_X64)
      hmx_init_fp16_scales(vtcm_bias);
   else
      memset(vtcm_bias, 0, HMX_BIAS_BYTES);
   memset(vtcm_output, 0, (size_t)written_len);
   memset(output, 0, (size_t)written_len);

   overhead = measure_timer_overhead_hmx();
   __asm__ volatile("" ::: "memory");
   start = read_cycles_hmx();
   for (int i = 0; i < repeats; ++i) {
      switch (mode) {
      case CDSP_PEAK_HMX_INT8_UB:
         hmx_int8_tile_ub(vtcm_activation, vtcm_weight, vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_CM_UB:
         hmx_int8_tile_cm_ub(vtcm_activation, vtcm_weight, vtcm_weight2,
                             vtcm_bias, vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UH:
         hmx_int8_tile_uh(vtcm_activation, vtcm_weight, vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_X16:
         hmx_int8_tile_ub_x16(vtcm_activation, vtcm_weight, vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_ADEEP32:
         hmx_int8_tile_ub_adeep32(vtcm_activation, vtcm_weight,
                                  vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_FULL_X16:
         hmx_int8_tile_ub_full_x16(vtcm_activation, vtcm_weight,
                                   vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_FULL_X32:
         hmx_int8_tile_ub_full_x32(vtcm_activation, vtcm_weight,
                                   vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_FULL_X64:
         hmx_int8_tile_ub_full_x64(vtcm_activation, vtcm_weight,
                                   vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT8_UB_WDEEP_FULL_X64:
         hmx_int8_tile_ub_wdeep_full_x64(vtcm_activation, vtcm_weight,
                                         vtcm_output);
         break;
      case CDSP_PEAK_HMX_FP16_HF_X64:
         hmx_fp16_tile_hf_x64(vtcm_activation, vtcm_weight, vtcm_bias,
                              vtcm_output);
         break;
      case CDSP_PEAK_HMX_INT4_UB_WN2X_FULL_X64:
         err = hmx_int4_tile_ub_wn2x_full_x64(vtcm_activation, vtcm_weight,
                                              vtcm_output);
         if (err) {
            hmx_resource_release(&resource);
            return err;
         }
         break;
      case CDSP_PEAK_HMX_INT8_UH2X2_FULL_X64:
         err = hmx_int8_tile_uh2x2_full_x64(vtcm_activation, vtcm_weight,
                                            vtcm_output);
         if (err) {
            hmx_resource_release(&resource);
            return err;
         }
         break;
      case CDSP_PEAK_HMX_INT8_UH_X16:
         hmx_int8_tile_uh_x16(vtcm_activation, vtcm_weight, vtcm_output);
         break;
      default:
         break;
      }
   }
   end = read_cycles_hmx();
   __asm__ volatile("" ::: "memory");

   *cycles = elapsed_cycles_hmx(start, end, overhead);
   memcpy(output, vtcm_output, (size_t)written_len);
   *checksum = checksum_hmx_output(output, written_len);
   hmx_resource_release(&resource);
   return AEE_SUCCESS;
}
