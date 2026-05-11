// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef CDSP_PEAK_HAP_POWER_H
#define CDSP_PEAK_HAP_POWER_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned char cdsp_peak_hap_bool_t;
typedef uint32_t cdsp_peak_hap_uint32_t;

#ifndef TRUE
#define TRUE 1
#endif

#define HAP_POWER_SET_HMX_V2_DEFINED

typedef enum {
   HAP_POWER_UNKNOWN_CLIENT_CLASS = 0x00,
   HAP_POWER_COMPUTE_CLIENT_CLASS = 0x04,
} HAP_power_app_type_payload;

typedef enum {
   HAP_DCVS_VCORNER_DISABLE = 0,
   HAP_DCVS_VCORNER_MAX = 255,
} HAP_dcvs_voltage_corner_t;

typedef enum {
   HAP_DCVS_EXP_VCORNER_MAX = 0xFFFF,
} HAP_dcvs_exp_voltage_corner_t;

typedef enum {
   HAP_CLK_PERF_HIGH = 0,
} HAP_clk_perf_mode_t;

typedef enum {
   HAP_DCVS_V2_PERFORMANCE_MODE = 0x10,
} HAP_power_dcvs_v2_payload_option;

typedef struct {
   HAP_dcvs_voltage_corner_t target_corner;
   HAP_dcvs_voltage_corner_t min_corner;
   HAP_dcvs_voltage_corner_t max_corner;
   cdsp_peak_hap_uint32_t param1;
   cdsp_peak_hap_uint32_t param2;
   cdsp_peak_hap_uint32_t param3;
} HAP_core_params_t;

typedef HAP_core_params_t HAP_bus_params_t;

typedef struct {
   cdsp_peak_hap_uint32_t param1;
   cdsp_peak_hap_uint32_t param2;
   cdsp_peak_hap_uint32_t param3;
   cdsp_peak_hap_uint32_t param4;
   cdsp_peak_hap_uint32_t param5;
   cdsp_peak_hap_uint32_t param6;
} HAP_dcvs_v3_params_t;

typedef struct {
   cdsp_peak_hap_bool_t set_dcvs_enable;
   cdsp_peak_hap_bool_t dcvs_enable;
   HAP_power_dcvs_v2_payload_option dcvs_option;
   cdsp_peak_hap_bool_t set_latency;
   cdsp_peak_hap_uint32_t latency;
   cdsp_peak_hap_bool_t set_core_params;
   HAP_core_params_t core_params;
   cdsp_peak_hap_bool_t set_bus_params;
   HAP_bus_params_t bus_params;
   cdsp_peak_hap_bool_t set_dcvs_v3_params;
   HAP_dcvs_v3_params_t dcvs_v3_params;
   cdsp_peak_hap_bool_t set_sleep_disable;
   unsigned char sleep_disable;
} HAP_power_dcvs_v3_payload;

typedef struct {
   cdsp_peak_hap_bool_t power_up;
} HAP_power_hvx_payload;

typedef HAP_power_hvx_payload HAP_power_hmx_payload;

typedef struct {
   cdsp_peak_hap_bool_t set_power;
   cdsp_peak_hap_bool_t power_up;
   cdsp_peak_hap_bool_t set_clock;
   cdsp_peak_hap_bool_t pick_default;
   HAP_dcvs_exp_voltage_corner_t target_corner;
   HAP_dcvs_exp_voltage_corner_t min_corner;
   HAP_dcvs_exp_voltage_corner_t max_corner;
   HAP_clk_perf_mode_t perf_mode;
   cdsp_peak_hap_uint32_t freq_mhz;
   cdsp_peak_hap_uint32_t floor_freq_mhz;
   cdsp_peak_hap_uint32_t param1;
   cdsp_peak_hap_uint32_t param2;
   cdsp_peak_hap_uint32_t param3;
} HAP_power_hmx_payload_v2;

typedef enum {
   HAP_power_set_HVX = 2,
   HAP_power_set_apptype = 3,
   HAP_power_set_DCVS_v3 = 12,
   HAP_power_set_HMX = 13,
   HAP_power_set_HMX_v2 = 14,
} HAP_Power_request_type;

typedef struct {
   HAP_Power_request_type type;
   union {
      HAP_power_app_type_payload apptype;
      HAP_power_dcvs_v3_payload dcvs_v3;
      HAP_power_hvx_payload hvx;
      HAP_power_hmx_payload hmx;
      HAP_power_hmx_payload_v2 hmx_v2;
      uint64_t align;
      unsigned char pad[112];
   };
} HAP_power_request_t;

_Static_assert(sizeof(HAP_core_params_t) == 16,
               "unexpected HAP_core_params_t size");
_Static_assert(sizeof(HAP_power_dcvs_v3_payload) == 80,
               "unexpected HAP_power_dcvs_v3_payload size");
_Static_assert(sizeof(HAP_power_hmx_payload_v2) == 32,
               "unexpected HAP_power_hmx_payload_v2 size");
_Static_assert(offsetof(HAP_power_request_t, dcvs_v3) == 8,
               "unexpected HAP_power_request_t payload offset");
_Static_assert(sizeof(HAP_power_request_t) == 120,
               "unexpected HAP_power_request_t size");

int HAP_power_set(void *context, HAP_power_request_t *request);

#endif
