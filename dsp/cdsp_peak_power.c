// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <stdint.h>
#include <string.h>

#include "AEEStdErr.h"
#include "cdsp_peak.h"
#include "cdsp_peak_hap_power.h"

#define CDSP_PEAK_POWER_NONE 0
#define CDSP_PEAK_POWER_MAX 1

#define CDSP_PEAK_POWER_APPTYPE 0x01
#define CDSP_PEAK_POWER_DCVS_MAX 0x02
#define CDSP_PEAK_POWER_HVX 0x04
#define CDSP_PEAK_POWER_HMX 0x08
#define CDSP_PEAK_POWER_HMX_V2 0x10

#define CDSP_PEAK_POWER_STEP_APPTYPE 1
#define CDSP_PEAK_POWER_STEP_DCVS_MAX 2
#define CDSP_PEAK_POWER_STEP_HVX 3
#define CDSP_PEAK_POWER_STEP_HMX 4
#define CDSP_PEAK_POWER_STEP_HMX_V2 5

#pragma weak HAP_power_set

static int apply_power_request(void *context, HAP_power_request_t *request)
{
   if (!HAP_power_set)
      return AEE_EUNSUPPORTED;
   return HAP_power_set(context, request);
}

static int set_compute_client(void *context)
{
   HAP_power_request_t request;

   memset(&request, 0, sizeof(request));
   request.type = HAP_power_set_apptype;
   request.apptype = HAP_POWER_COMPUTE_CLIENT_CLASS;
   return apply_power_request(context, &request);
}

static int set_dcvs_max(void *context)
{
   HAP_power_request_t request;

   memset(&request, 0, sizeof(request));
   request.type = HAP_power_set_DCVS_v3;
   request.dcvs_v3.set_dcvs_enable = TRUE;
   request.dcvs_v3.dcvs_enable = TRUE;
   request.dcvs_v3.dcvs_option = HAP_DCVS_V2_PERFORMANCE_MODE;
   request.dcvs_v3.set_bus_params = TRUE;
   request.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.set_core_params = TRUE;
   request.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_MAX;
   request.dcvs_v3.set_sleep_disable = TRUE;
   request.dcvs_v3.sleep_disable = TRUE;
   return apply_power_request(context, &request);
}

static int set_hvx_power(void *context)
{
   HAP_power_request_t request;

   memset(&request, 0, sizeof(request));
   request.type = HAP_power_set_HVX;
   request.hvx.power_up = TRUE;
   return apply_power_request(context, &request);
}

static int set_hmx_power(void *context)
{
   HAP_power_request_t request;

   memset(&request, 0, sizeof(request));
   request.type = HAP_power_set_HMX;
   request.hmx.power_up = TRUE;
   return apply_power_request(context, &request);
}

#if defined(__HVX_ARCH__) && __HVX_ARCH__ >= 75 && \
   defined(HAP_POWER_SET_HMX_V2_DEFINED)
static int set_hmx_v2_power(void *context)
{
   HAP_power_request_t request;

   memset(&request, 0, sizeof(request));
   request.type = HAP_power_set_HMX_v2;
   request.hmx_v2.set_clock = TRUE;
   request.hmx_v2.target_corner = HAP_DCVS_EXP_VCORNER_MAX;
   request.hmx_v2.min_corner = HAP_DCVS_EXP_VCORNER_MAX;
   request.hmx_v2.max_corner = HAP_DCVS_EXP_VCORNER_MAX;
   request.hmx_v2.perf_mode = HAP_CLK_PERF_HIGH;
   return apply_power_request(context, &request);
}
#endif

static void record_failure(int step, int err, int *failed_step,
                           int *failed_error)
{
   *failed_step = step;
   *failed_error = err;
}

int cdsp_peak_set_power(remote_handle64 handle, int mode, int *applied_mask,
                        int *failed_step, int *failed_error)
{
   void *context = (void *)(uintptr_t)handle;
   int err;

   if (!applied_mask || !failed_step || !failed_error)
      return AEE_EBADPARM;

   *applied_mask = 0;
   *failed_step = 0;
   *failed_error = 0;

   if (mode == CDSP_PEAK_POWER_NONE)
      return AEE_SUCCESS;
   if (mode != CDSP_PEAK_POWER_MAX)
      return AEE_EBADPARM;

   err = set_compute_client(context);
   if (err) {
      record_failure(CDSP_PEAK_POWER_STEP_APPTYPE, err, failed_step,
                     failed_error);
      return AEE_SUCCESS;
   }
   *applied_mask |= CDSP_PEAK_POWER_APPTYPE;

   err = set_dcvs_max(context);
   if (err) {
      record_failure(CDSP_PEAK_POWER_STEP_DCVS_MAX, err, failed_step,
                     failed_error);
      return AEE_SUCCESS;
   }
   *applied_mask |= CDSP_PEAK_POWER_DCVS_MAX;

   err = set_hvx_power(context);
   if (err) {
      record_failure(CDSP_PEAK_POWER_STEP_HVX, err, failed_step,
                     failed_error);
      return AEE_SUCCESS;
   }
   *applied_mask |= CDSP_PEAK_POWER_HVX;

   err = set_hmx_power(context);
   if (err) {
      record_failure(CDSP_PEAK_POWER_STEP_HMX, err, failed_step,
                     failed_error);
      return AEE_SUCCESS;
   }
   *applied_mask |= CDSP_PEAK_POWER_HMX;

#if defined(__HVX_ARCH__) && __HVX_ARCH__ >= 75 && \
   defined(HAP_POWER_SET_HMX_V2_DEFINED)
   err = set_hmx_v2_power(context);
   if (err) {
      record_failure(CDSP_PEAK_POWER_STEP_HMX_V2, err, failed_step,
                     failed_error);
      return AEE_SUCCESS;
   }
   *applied_mask |= CDSP_PEAK_POWER_HMX_V2;
#endif

   return AEE_SUCCESS;
}
