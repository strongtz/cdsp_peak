// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "AEEStdErr.h"
#include "cdsp_peak.h"
#include "remote.h"
#include "rpcmem.h"

#define KERNEL_FP32 0
#define KERNEL_FP16 1
#define KERNEL_INT8_FIXED 2
#define KERNEL_INT8_ADD_FIXED 3
#define KERNEL_QF16 4
#define KERNEL_QF32 5

#define HMX_INT8_UB 0
#define HMX_INT8_CM_UB 1
#define HMX_INT8_UH 2
#define HMX_INT8_UB_X16 10
#define HMX_INT8_UH_X16 11
#define HMX_INT8_UB_FULL_X16 12
#define HMX_INT8_UB_FULL_X32 13
#define HMX_INT8_UB_FULL_X64 14
#define HMX_INT8_UH2X2_FULL_X64 15
#define HMX_INT8_UB_ADEEP32 16
#define HMX_INT8_UB_WDEEP_FULL_X64 17
#define HMX_FP16_HF_X64 18
#define HMX_INT4_UB_WN2X_FULL_X64 19
#define HMX_PROBE_RESOURCE 100
#define HMX_PROBE_CACHED 101
#define HMX_PROBE_LOCK 102
#define HMX_TILE_U8_BYTES 1024
#define HMX_TILE_U16_BYTES 2048
#define HMX_DEEP_TILES 32
#define HMX_OUTPUT_BYTES 2048
#define HMX_OUTPUT_2X2_BYTES 8192

#define POWER_NONE 0
#define POWER_MAX 1

#define UNSIGNED_PD_OFF 0
#define UNSIGNED_PD_REQUIRED 1
#define UNSIGNED_PD_OPTIONAL 2

#define POWER_STEP_APPTYPE 1
#define POWER_STEP_DCVS_MAX 2
#define POWER_STEP_HVX 3
#define POWER_STEP_HMX 4
#define POWER_STEP_HMX_V2 5

#define CDSP_PEAK_SKEL_URI_FORMAT \
   "file:///libcdsp_peak_skel_%s.so?cdsp_peak_skel_handle_invoke" \
   "&_modver=1.0&_idlver=0.1.0"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ITERATION_GROUP_NONE 0
#define ITERATION_GROUP_INT8 1
#define MAX_COMPUTE_ITERATIONS 0x30000000
#define MAX_HMX_REPEATS 0x1000000
#define MAX_THREADS 256

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct options {
   int domain;
   int unsigned_pd;
   int reset;
   int min_ms;
   int iterations;
   int threads;
   int threads_auto;
   int power_mode;
   size_t size_mib;
   const char *scenario_filter;
};

struct compute_scenario {
   const char *name;
   const char *unit;
   int kernel;
   double ops_per_iter;
   int start_iterations;
   int iteration_group;
};

struct bench_result {
   uint64_t count;
   uint64_t cycles;
   uint64_t checksum;
   double elapsed_ms;
};

struct mem_scenario {
   const char *name;
   int kind;
   double bytes_scale;
};

struct hmx_scenario {
   const char *name;
   int mode;
   const char *unit;
   double ops_per_repeat;
   int start_repeats;
   size_t activation_bytes;
   size_t weight_bytes;
   size_t output_bytes;
   int default_enabled;
   int default_min_arch;
};

static const struct compute_scenario compute_scenarios[] = {
   {"fp32-vmuladd", "GFLOPS", KERNEL_FP32, 8.0 * 32.0 * 2.0, 10000,
    ITERATION_GROUP_NONE},
   {"fp16-vmpyacc", "GFLOPS", KERNEL_FP16, 8.0 * 64.0 * 2.0, 10000,
    ITERATION_GROUP_NONE},
   {"qf16-vmpyadd", "GFLOPS", KERNEL_QF16, 8.0 * 64.0 * 2.0, 10000,
    ITERATION_GROUP_NONE},
   {"qf32-vmpyadd", "GFLOPS", KERNEL_QF32, 8.0 * 32.0 * 2.0, 10000,
    ITERATION_GROUP_NONE},
   {"int8-vrmpyacc", "GIOPS", KERNEL_INT8_FIXED, 8.0 * 32.0 * 4.0 * 2.0,
    10000, ITERATION_GROUP_INT8},
   {"int8-vrmpy-add", "GIOPS", KERNEL_INT8_ADD_FIXED,
    8.0 * 32.0 * 4.0 * 2.0, 10000, ITERATION_GROUP_INT8},
};

static const struct mem_scenario mem_scenarios[] = {
   {"mem-read", 0, 1.0},
   {"mem-write", 1, 1.0},
   {"mem-copy", 2, 2.0},
};

static const struct hmx_scenario hmx_scenarios[] = {
   {"hmx-resource", HMX_PROBE_RESOURCE, "probe", 0.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 16, 1, 0},
   {"hmx-cached", HMX_PROBE_CACHED, "probe", 0.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 16, 1, 0},
   {"hmx-lock", HMX_PROBE_LOCK, "probe", 0.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 16, 1, 0},
   {"hmx-int8-ub", HMX_INT8_UB, "GIOPS", 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 1, 0},
   {"hmx-int8-cm-ub", HMX_INT8_CM_UB, "GIOPS", 32.0 * 32.0 * 32.0 * 2.0,
    1, HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 1, 0},
   {"hmx-int8-uh", HMX_INT8_UH, "GIOPS", 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-int8-ub-x16", HMX_INT8_UB_X16, "GIOPS",
    16.0 * 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 1, 0},
   {"hmx-int8-uh-x16", HMX_INT8_UH_X16, "GIOPS",
    16.0 * 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-int8-ub-full-x16", HMX_INT8_UB_FULL_X16, "GIOPS",
    16.0 * 32.0 * 64.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-int8-ub-full-x32", HMX_INT8_UB_FULL_X32, "GIOPS",
    32.0 * 32.0 * 64.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-int8-ub-full-x64", HMX_INT8_UB_FULL_X64, "GIOPS",
    64.0 * 32.0 * 64.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-int8-uh2x2-full-x64", HMX_INT8_UH2X2_FULL_X64, "GIOPS",
    64.0 * 32.0 * 128.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_2X2_BYTES, 1, 69},
   {"hmx-int8-ub-adeep32", HMX_INT8_UB_ADEEP32, "GIOPS",
    (double)HMX_DEEP_TILES * 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_DEEP_TILES * HMX_TILE_U8_BYTES,
    HMX_DEEP_TILES * HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, 1, 0},
   {"hmx-int8-ub-wdeep-full-x64", HMX_INT8_UB_WDEEP_FULL_X64, "GIOPS",
    64.0 * 32.0 * 64.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, 2 * HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 0},
   {"hmx-fp16-hf-x64", HMX_FP16_HF_X64, "GFLOPS",
    64.0 * 32.0 * 32.0 * 32.0 * 2.0, 1,
    HMX_TILE_U16_BYTES, HMX_TILE_U16_BYTES, HMX_TILE_U16_BYTES, 1, 0},
   {"hmx-int4-ub-wn2x-full-x64", HMX_INT4_UB_WN2X_FULL_X64, "GIOPS",
    64.0 * 32.0 * 128.0 * 32.0 * 2.0, 1,
    HMX_TILE_U8_BYTES, HMX_TILE_U8_BYTES, HMX_OUTPUT_BYTES, 1, 73},
};

static double now_ms(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static uint64_t mix_checksum(uint64_t acc, uint64_t value, uint64_t index)
{
   acc ^= value + 0x9e3779b97f4a7c15ULL + (index << 6) + (index >> 2);
   acc *= 0x100000001b3ULL;
   return acc;
}

static const char *domain_suffix(int domain)
{
   switch (domain) {
   case ADSP_DOMAIN_ID:
      return ADSP_DOMAIN;
   case MDSP_DOMAIN_ID:
      return MDSP_DOMAIN;
   case SDSP_DOMAIN_ID:
      return SDSP_DOMAIN;
   case CDSP_DOMAIN_ID:
      return CDSP_DOMAIN;
   case CDSP1_DOMAIN_ID:
      return CDSP1_DOMAIN;
   default:
      return CDSP_DOMAIN;
   }
}

static const char *domain_name(int domain)
{
   switch (domain) {
   case ADSP_DOMAIN_ID:
      return "adsp";
   case MDSP_DOMAIN_ID:
      return "mdsp";
   case SDSP_DOMAIN_ID:
      return "sdsp";
   case CDSP_DOMAIN_ID:
      return "cdsp";
   case CDSP1_DOMAIN_ID:
      return "cdsp1";
   default:
      return "unknown";
   }
}

static const char *power_mode_name(int mode)
{
   switch (mode) {
   case POWER_NONE:
      return "none";
   case POWER_MAX:
      return "max";
   default:
      return "unknown";
   }
}

static const char *power_step_name(int step)
{
   switch (step) {
   case POWER_STEP_APPTYPE:
      return "apptype";
   case POWER_STEP_DCVS_MAX:
      return "dcvs-max";
   case POWER_STEP_HVX:
      return "hvx";
   case POWER_STEP_HMX:
      return "hmx";
   case POWER_STEP_HMX_V2:
      return "hmx-v2";
   default:
      return "unknown";
   }
}

static uint32_t normalize_hexagon_arch(uint32_t arch)
{
   uint32_t low = arch & 0xffu;

   if (arch >= 60 && arch <= 99)
      return arch;
   if (low >= 0x60 && low <= 0x99)
      return ((low >> 4) * 10u) + (low & 0xfu);
   return arch;
}

static const char *skel_arch_suffix(uint32_t arch)
{
   switch (normalize_hexagon_arch(arch)) {
   case 81:
      return "v81";
   case 79:
      return "v79";
   case 75:
      return "v75";
   case 73:
      return "v73";
   case 69:
      return "v69";
   case 68:
      return "v68";
   default:
      break;
   }
   return NULL;
}

static int make_uri(int domain, uint32_t arch, char **uri)
{
   const char *base = getenv("CDSP_PEAK_URI");
   const char *suffix = domain_suffix(domain);
   const char *arch_suffix = skel_arch_suffix(arch);
   size_t len;

   if (base) {
      len = strlen(base) + strlen(suffix) + 1;
      *uri = (char *)malloc(len);
      if (!*uri)
         return -ENOMEM;
      snprintf(*uri, len, "%s%s", base, suffix);
      return 0;
   }

   if (!arch_suffix) {
      fprintf(stderr,
              "unsupported Hexagon ARCH_VER 0x%x (normalized v%u);"
              " no exact skel build is available\n",
              arch, normalize_hexagon_arch(arch));
      return -ENOTSUP;
   }

   len = (size_t)snprintf(NULL, 0, CDSP_PEAK_SKEL_URI_FORMAT "%s",
                          arch_suffix, suffix) +
         1u;
   *uri = (char *)malloc(len);
   if (!*uri)
      return -ENOMEM;

   snprintf(*uri, len, CDSP_PEAK_SKEL_URI_FORMAT "%s", arch_suffix, suffix);
   return 0;
}

static int executable_dir(char *dir, size_t dir_size)
{
   ssize_t len;
   char *slash;

   if (!dir || dir_size == 0)
      return -EINVAL;

   len = readlink("/proc/self/exe", dir, dir_size - 1);
   if (len < 0)
      return -errno;
   if (len == 0 || (size_t)len >= dir_size - 1)
      return -ENAMETOOLONG;
   dir[len] = '\0';

   slash = strrchr(dir, '/');
   if (!slash)
      return -EINVAL;
   if (slash == dir)
      slash[1] = '\0';
   else
      *slash = '\0';

   return 0;
}

static bool executable_dir_has_skel(const char *dir)
{
   static const char *skel_names[] = {
      "libcdsp_peak_skel_v68.so",
      "libcdsp_peak_skel_v69.so",
      "libcdsp_peak_skel_v73.so",
      "libcdsp_peak_skel_v75.so",
      "libcdsp_peak_skel_v79.so",
      "libcdsp_peak_skel_v81.so",
   };
   char path[PATH_MAX];
   size_t i;

   for (i = 0; i < ARRAY_SIZE(skel_names); ++i) {
      int needed = snprintf(path, sizeof(path), "%s/%s", dir, skel_names[i]);
      if (needed > 0 && (size_t)needed < sizeof(path) &&
          access(path, R_OK) == 0)
         return true;
   }

   return false;
}

static void set_env_path(const char *name, const char *dir)
{
   if (!dir || !dir[0])
      return;
   (void)setenv(name, dir, 1);
}

static void add_executable_dir_to_dsp_paths(void)
{
   char dir[PATH_MAX];

   if (executable_dir(dir, sizeof(dir)))
      return;
   if (!executable_dir_has_skel(dir))
      return;

   set_env_path("ADSP_LIBRARY_PATH", dir);
   set_env_path("DSP_LIBRARY_PATH", dir);
}

static int enable_unsigned_pd(int domain, int enable)
{
   struct remote_rpc_control_unsigned_module request = {
      .domain = domain,
      .enable = enable,
   };

   return remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, &request,
                                 sizeof(request));
}

static void reset_domain_if_requested(int domain, int reset)
{
   struct remote_rpc_process_clean_params request = {
      .domain = domain,
   };
   int err;

   if (!reset)
      return;

   err = remote_session_control(FASTRPC_REMOTE_PROCESS_KILL, &request,
                                sizeof(request));
   if (err)
      fprintf(stderr, "warning: domain reset returned 0x%x\n", err);
}

static int query_dsp_capability(int domain, int attribute, uint32_t *capability)
{
   fastrpc_capability request = {
      .domain = (uint32_t)domain,
      .attribute_ID = (uint32_t)attribute,
      .capability = 0,
   };
   int err = remote_handle_control(DSPRPC_GET_DSP_INFO, &request,
                                   sizeof(request));

   if (!err)
      *capability = request.capability;
   return err;
}

static int apply_power_config(const remote_handle64 *handles, int nthreads,
                              int mode, int *applied_mask_out)
{
   int applied_union = 0;

   *applied_mask_out = 0;
   if (mode == POWER_NONE)
      return 0;

   for (int i = 0; i < nthreads; ++i) {
      int applied_mask = 0;
      int failed_step = 0;
      int failed_error = 0;
      int err = cdsp_peak_set_power(handles[i], mode, &applied_mask,
                                    &failed_step, &failed_error);
      if (err) {
         fprintf(stderr, "cdsp_peak_set_power thread %d failed: 0x%x\n", i,
                 err);
         return err;
      }
      applied_union |= applied_mask;
      if (failed_step) {
         fprintf(stderr,
                 "cdsp_peak_set_power thread %d failed at %s: 0x%x"
                 " (applied=0x%x)\n",
                 i, power_step_name(failed_step), (unsigned)failed_error,
                 applied_mask);
         return failed_error ? failed_error : AEE_EFAILED;
      }
   }

   *applied_mask_out = applied_union;
   return 0;
}

static void usage(const char *prog)
{
   printf("Usage: %s [--scenario all|name[,name...]] [--size-mib N] [--min-ms N]\n"
          "          [--iterations N] [--threads N]\n"
          "          [--domain N] [--unsigned-pd 0|1|optional]"
          " [--power none|max]\n"
          "          [--reset]\n\n"
          "Scenarios: rpc-null, fp32-vmuladd, fp16-vmpyacc,\n"
          "           qf16-vmpyadd, qf32-vmpyadd,\n"
          "           int8-vrmpyacc, int8-vrmpy-add,\n"
          "           mem-read, mem-write, mem-copy, all\n"
          "HMX: hmx-resource, hmx-cached,\n"
          "           hmx-lock, hmx-int8-ub, hmx-int8-cm-ub, hmx-int8-uh,\n"
          "           hmx-int8-ub-x16, hmx-int8-uh-x16,"
          " hmx-int8-ub-full-x16, hmx-int8-ub-full-x32,"
          " hmx-int8-ub-full-x64, hmx-int8-uh2x2-full-x64,\n"
          "           hmx-int8-ub-adeep32, hmx-int8-ub-wdeep-full-x64,\n"
          "           hmx-fp16-hf-x64, hmx-int4-ub-wn2x-full-x64\n"
          "Default thread count is HVX_SUPPORT_128B, falling back to 1.\n"
          "Default power mode is max: compute client, DCVS max, HVX on, HMX on.\n",
          prog);
}

static int parse_int_arg(const char *value, const char *name)
{
   char *end = NULL;
   long parsed = strtol(value, &end, 0);

   if (!value[0] || (end && *end)) {
      fprintf(stderr, "invalid %s: %s\n", name, value);
      exit(2);
   }
   return (int)parsed;
}

static int parse_unsigned_pd_arg(const char *value)
{
   if (!strcmp(value, "0") || !strcmp(value, "off") ||
       !strcmp(value, "none"))
      return UNSIGNED_PD_OFF;
   if (!strcmp(value, "1") || !strcmp(value, "on") ||
       !strcmp(value, "required"))
      return UNSIGNED_PD_REQUIRED;
   if (!strcmp(value, "optional") || !strcmp(value, "auto"))
      return UNSIGNED_PD_OPTIONAL;

   fprintf(stderr, "invalid --unsigned-pd: %s\n", value);
   exit(2);
}

static int parse_power_arg(const char *value)
{
   if (!strcmp(value, "none") || !strcmp(value, "off") || !strcmp(value, "0"))
      return POWER_NONE;
   if (!strcmp(value, "max") || !strcmp(value, "on") || !strcmp(value, "1"))
      return POWER_MAX;

   fprintf(stderr, "invalid --power: %s\n", value);
   exit(2);
}

static size_t parse_size_arg(const char *value, const char *name)
{
   char *end = NULL;
   unsigned long long parsed = strtoull(value, &end, 0);

   if (!value[0] || (end && *end)) {
      fprintf(stderr, "invalid %s: %s\n", name, value);
      exit(2);
   }
   return (size_t)parsed;
}

static void parse_options(int argc, char **argv, struct options *opt)
{
   opt->domain = CDSP_DOMAIN_ID;
   opt->unsigned_pd = UNSIGNED_PD_REQUIRED;
   opt->reset = 0;
   opt->min_ms = 300;
   opt->iterations = 0;
   opt->threads = 0;
   opt->threads_auto = 1;
   opt->power_mode = POWER_MAX;
   opt->size_mib = 64;
   opt->scenario_filter = "all";

   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--scenario") && i + 1 < argc) {
         opt->scenario_filter = argv[++i];
      } else if (!strcmp(argv[i], "--size-mib") && i + 1 < argc) {
         opt->size_mib = parse_size_arg(argv[++i], "--size-mib");
      } else if (!strcmp(argv[i], "--min-ms") && i + 1 < argc) {
         opt->min_ms = parse_int_arg(argv[++i], "--min-ms");
      } else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
         opt->iterations = parse_int_arg(argv[++i], "--iterations");
      } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
         opt->threads = parse_int_arg(argv[++i], "--threads");
         opt->threads_auto = opt->threads <= 0;
      } else if (!strcmp(argv[i], "--domain") && i + 1 < argc) {
         opt->domain = parse_int_arg(argv[++i], "--domain");
      } else if (!strcmp(argv[i], "--unsigned-pd") && i + 1 < argc) {
         opt->unsigned_pd = parse_unsigned_pd_arg(argv[++i]);
      } else if (!strcmp(argv[i], "--power") && i + 1 < argc) {
         opt->power_mode = parse_power_arg(argv[++i]);
      } else if (!strcmp(argv[i], "--reset")) {
         opt->reset = 1;
      } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
         usage(argv[0]);
         exit(0);
      } else {
         usage(argv[0]);
         exit(2);
      }
   }

   if (opt->min_ms <= 0)
      opt->min_ms = 1;
   if (opt->iterations < 0)
      opt->iterations = 0;
   if (opt->threads > MAX_THREADS)
      opt->threads = MAX_THREADS;
   if (opt->size_mib == 0)
      opt->size_mib = 1;
}

static void resolve_thread_count(struct options *opt, uint32_t hvx_64b,
                                 uint32_t hvx_128b)
{
   uint32_t auto_threads = hvx_128b ? hvx_128b : hvx_64b;

   if (opt->threads_auto)
      opt->threads = auto_threads > 0 ? (int)auto_threads : 1;
   if (opt->threads <= 0)
      opt->threads = 1;
   if (opt->threads > MAX_THREADS)
      opt->threads = MAX_THREADS;
}

static bool scenario_enabled(const struct options *opt, const char *name)
{
   const char *filter = opt->scenario_filter;
   size_t name_len = strlen(name);

   if (!strcmp(filter, "all"))
      return true;

   while (*filter) {
      const char *comma = strchr(filter, ',');
      size_t len = comma ? (size_t)(comma - filter) : strlen(filter);

      if (len == name_len && !strncmp(filter, name, len))
         return true;
      if (!comma)
         break;
      filter = comma + 1;
   }

   return false;
}

static bool hmx_scenario_enabled(const struct options *opt,
                                 const struct hmx_scenario *scenario,
                                 int arch)
{
   if (!strcmp(opt->scenario_filter, "all")) {
      if (!scenario->default_enabled)
         return false;
      return arch <= 0 || arch >= scenario->default_min_arch;
   }

   return scenario_enabled(opt, scenario->name);
}

static void print_header(void)
{
   printf("%-30s %7s %12s %12s %14s %14s %12s %18s\n",
          "scenario", "threads", "count", "host_ms", "dsp_cycles", "ops/cycle",
          "score", "checksum");
}

static void print_row(const char *scenario, int threads, uint64_t count,
                      double host_ms, uint64_t cycles, double ops_per_cycle,
                      double score, const char *unit, uint64_t checksum)
{
   printf("%-30s %7d %12" PRIu64 " %12.3f %14" PRIu64 " %14.3f %9.2f %-6s 0x%016" PRIx64 "\n",
          scenario, threads, count, host_ms, cycles, ops_per_cycle, score,
          unit, checksum);
}

struct rpc_thread_arg {
   remote_handle64 handle;
   int calls;
   int err;
   uint64_t checksum;
};

struct compute_thread_arg {
   remote_handle64 handle;
   const struct compute_scenario *scenario;
   int iterations;
   int err;
   uint64_t cycles;
   uint64_t checksum;
};

static void *rpc_thread_main(void *data)
{
   struct rpc_thread_arg *arg = (struct rpc_thread_arg *)data;
   int arch = 0;
   int hvx_bytes = 0;
   uint64_t overhead = 0;
   uint64_t current_cycles = 0;

   for (int i = 0; i < arg->calls; ++i) {
      arg->err = cdsp_peak_get_info(arg->handle, &arch, &hvx_bytes, &overhead,
                                    &current_cycles);
      if (arg->err) {
         arg->checksum = 0;
         return NULL;
      }
   }

   arg->checksum = current_cycles ^ overhead ^ (uint64_t)hvx_bytes;
   return NULL;
}

static void *compute_thread_main(void *data)
{
   struct compute_thread_arg *arg = (struct compute_thread_arg *)data;

   arg->err = cdsp_peak_bench_compute(arg->handle, arg->scenario->kernel,
                                      arg->iterations, &arg->cycles,
                                      &arg->checksum);
   return NULL;
}

static int measure_rpc_threads(const remote_handle64 *handles, int nthreads,
                               int calls, struct bench_result *result)
{
   struct rpc_thread_arg *args =
      (struct rpc_thread_arg *)calloc((size_t)nthreads, sizeof(*args));
   pthread_t *threads = NULL;
   int err = 0;
   int created = 0;
   double start;

   if (!args)
      return -ENOMEM;
   if (nthreads > 1) {
      threads = (pthread_t *)calloc((size_t)nthreads, sizeof(*threads));
      if (!threads) {
         free(args);
         return -ENOMEM;
      }
   }

   for (int i = 0; i < nthreads; ++i) {
      args[i].handle = handles[i];
      args[i].calls = calls;
   }

   start = now_ms();
   if (nthreads == 1) {
      rpc_thread_main(&args[0]);
   } else {
      for (int i = 0; i < nthreads; ++i) {
         err = pthread_create(&threads[i], NULL, rpc_thread_main, &args[i]);
         if (err)
            break;
         ++created;
      }
      for (int i = 0; i < created; ++i)
         pthread_join(threads[i], NULL);
   }
   result->elapsed_ms = now_ms() - start;

   result->count = (uint64_t)calls * (uint64_t)nthreads;
   result->cycles = 0;
   result->checksum = 0xcbf29ce484222325ULL;

   if (!err) {
      for (int i = 0; i < nthreads; ++i) {
         if (args[i].err) {
            err = args[i].err;
            break;
         }
         result->checksum = mix_checksum(result->checksum, args[i].checksum,
                                         (uint64_t)i);
      }
   }

   free(threads);
   free(args);
   return err;
}

static int run_rpc_null(const remote_handle64 *handles, const struct options *opt)
{
   int calls = 1;
   int err;
   struct bench_result result = {0};

   for (;;) {
      err = measure_rpc_threads(handles, opt->threads, calls, &result);
      if (err)
         return err;

      if (result.elapsed_ms >= (double)opt->min_ms || calls >= (1 << 24))
         break;
      calls *= 2;
   }

   print_row("rpc-null", opt->threads, result.count, result.elapsed_ms, 0, 0.0,
             result.elapsed_ms * 1000.0 / (double)result.count, "us/call",
             result.checksum);
   return 0;
}

static int measure_compute_threads(const remote_handle64 *handles, int nthreads,
                                   const struct compute_scenario *scenario,
                                   int iterations, struct bench_result *result)
{
   struct compute_thread_arg *args =
      (struct compute_thread_arg *)calloc((size_t)nthreads, sizeof(*args));
   pthread_t *threads = NULL;
   int err = 0;
   int created = 0;
   double start;

   if (!args)
      return -ENOMEM;
   if (nthreads > 1) {
      threads = (pthread_t *)calloc((size_t)nthreads, sizeof(*threads));
      if (!threads) {
         free(args);
         return -ENOMEM;
      }
   }

   for (int i = 0; i < nthreads; ++i) {
      args[i].handle = handles[i];
      args[i].scenario = scenario;
      args[i].iterations = iterations;
   }

   start = now_ms();
   if (nthreads == 1) {
      compute_thread_main(&args[0]);
   } else {
      for (int i = 0; i < nthreads; ++i) {
         err = pthread_create(&threads[i], NULL, compute_thread_main, &args[i]);
         if (err)
            break;
         ++created;
      }
      for (int i = 0; i < created; ++i)
         pthread_join(threads[i], NULL);
   }
   result->elapsed_ms = now_ms() - start;

   result->count = (uint64_t)iterations * (uint64_t)nthreads;
   result->cycles = 0;
   result->checksum = 0xcbf29ce484222325ULL;

   if (!err) {
      for (int i = 0; i < nthreads; ++i) {
         if (args[i].err) {
            err = args[i].err;
            break;
         }
         if (args[i].cycles > result->cycles)
            result->cycles = args[i].cycles;
         result->checksum = mix_checksum(result->checksum, args[i].checksum,
                                         (uint64_t)i);
      }
   }

   free(threads);
   free(args);
   return err;
}

static void print_compute_result(const struct compute_scenario *scenario,
                                 int nthreads, const struct bench_result *result)
{
   double ops = scenario->ops_per_iter * (double)result->count;
   double score = ops / (result->elapsed_ms / 1000.0) / 1.0e9;
   double ops_per_cycle = result->cycles ? ops / (double)result->cycles : 0.0;

   print_row(scenario->name, nthreads, result->count, result->elapsed_ms,
             result->cycles, ops_per_cycle, score, scenario->unit,
             result->checksum);
}

static int run_compute(const remote_handle64 *handles, const struct options *opt,
                       const struct compute_scenario *scenario)
{
   int iterations = opt->iterations > 0 ? opt->iterations : scenario->start_iterations;
   int err;
   struct bench_result result = {0};

   for (;;) {
      err = measure_compute_threads(handles, opt->threads, scenario, iterations,
                                    &result);
      if (err)
         return err;

      if (opt->iterations > 0 || result.elapsed_ms >= (double)opt->min_ms ||
          iterations > MAX_COMPUTE_ITERATIONS)
         break;
      iterations *= 2;
   }

   print_compute_result(scenario, opt->threads, &result);
   return 0;
}

static int choose_group_start_iterations(const struct options *opt, int group)
{
   int iterations = opt->iterations > 0 ? opt->iterations : 0;

   if (iterations > 0)
      return iterations;

   for (unsigned i = 0; i < ARRAY_SIZE(compute_scenarios); ++i) {
      if (compute_scenarios[i].iteration_group != group)
         continue;
      if (!scenario_enabled(opt, compute_scenarios[i].name))
         continue;
      if (compute_scenarios[i].start_iterations > iterations)
         iterations = compute_scenarios[i].start_iterations;
   }

   return iterations;
}

static int run_compute_group(const remote_handle64 *handles,
                             const struct options *opt,
                             int group)
{
   struct bench_result results[ARRAY_SIZE(compute_scenarios)] = {0};
   int iterations = choose_group_start_iterations(opt, group);
   int err;

   if (iterations <= 0)
      return 0;

   for (;;) {
      double min_elapsed = 1.0e300;
      unsigned measured = 0;

      for (unsigned i = 0; i < ARRAY_SIZE(compute_scenarios); ++i) {
         if (compute_scenarios[i].iteration_group != group)
            continue;
         if (!scenario_enabled(opt, compute_scenarios[i].name))
            continue;

         err = measure_compute_threads(handles, opt->threads,
                                       &compute_scenarios[i], iterations,
                                       &results[i]);
         if (err)
            return err;
         if (results[i].elapsed_ms < min_elapsed)
            min_elapsed = results[i].elapsed_ms;
         ++measured;
      }

      if (!measured || opt->iterations > 0 || min_elapsed >= (double)opt->min_ms ||
          iterations > MAX_COMPUTE_ITERATIONS)
         break;
      iterations *= 2;
   }

   for (unsigned i = 0; i < ARRAY_SIZE(compute_scenarios); ++i) {
      if (compute_scenarios[i].iteration_group != group)
         continue;
      if (!scenario_enabled(opt, compute_scenarios[i].name))
         continue;
      print_compute_result(&compute_scenarios[i], opt->threads, &results[i]);
   }

   return 0;
}

static void fill_hmx_fp16_tile(uint8_t *data, size_t bytes)
{
   for (size_t i = 0; i + 1 < bytes; i += 2) {
      data[i] = 0x00;
      data[i + 1] = 0x3c;
   }
}

static void fill_hmx_inputs(int mode, uint8_t *activation, uint8_t *weight,
                            uint8_t *output, size_t activation_bytes,
                            size_t weight_bytes, size_t output_bytes)
{
   if (mode == HMX_FP16_HF_X64) {
      fill_hmx_fp16_tile(activation, activation_bytes);
      fill_hmx_fp16_tile(weight, weight_bytes);
   } else {
      memset(activation, 1, activation_bytes);
      memset(weight, 1, weight_bytes);
   }
   memset(output, 0, output_bytes);
}

static int run_hmx_int8(remote_handle64 handle, const struct options *opt,
                        const struct hmx_scenario *scenario,
                        uint8_t *activation, uint8_t *weight,
                        uint8_t *output)
{
   int repeats = opt->iterations > 0 ? opt->iterations : scenario->start_repeats;
   int err;
   struct bench_result result = {0};

   for (;;) {
      fill_hmx_inputs(scenario->mode, activation, weight, output,
                      scenario->activation_bytes, scenario->weight_bytes,
                      scenario->output_bytes);

      double start = now_ms();
      err = cdsp_peak_bench_hmx_int8(handle, activation,
                                     (int)scenario->activation_bytes,
                                     weight, (int)scenario->weight_bytes,
                                     output, (int)scenario->output_bytes,
                                     scenario->mode, repeats,
                                     &result.cycles, &result.checksum);
      result.elapsed_ms = now_ms() - start;
      result.count = (uint64_t)repeats;

      if (err)
         return err;
      if (scenario->ops_per_repeat == 0.0 || opt->iterations > 0 ||
          result.elapsed_ms >= (double)opt->min_ms ||
          repeats >= MAX_HMX_REPEATS)
         break;
      repeats *= 2;
   }

   double ops = scenario->ops_per_repeat * (double)result.count;
   double score = ops / (result.elapsed_ms / 1000.0) / 1.0e9;
   double ops_per_cycle = result.cycles ? ops / (double)result.cycles : 0.0;

   print_row(scenario->name, 1, result.count, result.elapsed_ms,
             result.cycles, ops_per_cycle, score, scenario->unit,
             result.checksum);
   return 0;
}

static int fill_source(uint8_t *src, size_t bytes)
{
   for (size_t i = 0; i < bytes; ++i)
      src[i] = (uint8_t)((i * 131u + 17u) & 0xffu);
   return 0;
}

static int verify_copy(const uint8_t *src, const uint8_t *dst, size_t bytes)
{
   size_t probes = bytes < 4096 ? bytes : 4096;

   for (size_t i = 0; i < probes; ++i) {
      size_t idx = (i * 2654435761u) % bytes;
      if (src[idx] != dst[idx])
         return -1;
   }
   return 0;
}

struct mem_thread_arg {
   remote_handle64 handle;
   const struct mem_scenario *scenario;
   uint8_t *src;
   uint8_t *dst;
   int len;
   int repeats;
   int err;
   uint64_t cycles;
   uint64_t checksum;
};

static void *mem_thread_main(void *data)
{
   struct mem_thread_arg *arg = (struct mem_thread_arg *)data;

   if (arg->scenario->kind == 0) {
      arg->err = cdsp_peak_bench_mem_read(arg->handle, arg->src, arg->len,
                                          arg->repeats, &arg->cycles,
                                          &arg->checksum);
   } else if (arg->scenario->kind == 1) {
      arg->err = cdsp_peak_bench_mem_write(arg->handle, arg->dst, arg->len,
                                           arg->repeats, &arg->cycles,
                                           &arg->checksum);
   } else {
      arg->err = cdsp_peak_bench_mem_copy(arg->handle, arg->src, arg->len,
                                          arg->dst, arg->len, arg->repeats,
                                          &arg->cycles, &arg->checksum);
      if (!arg->err && verify_copy(arg->src, arg->dst, (size_t)arg->len))
         arg->err = AEE_EFAILED;
   }

   return NULL;
}

static int measure_mem_threads(const remote_handle64 *handles, int nthreads,
                               const struct mem_scenario *scenario,
                               uint8_t **srcs, uint8_t **dsts, size_t bytes,
                               int repeats, struct bench_result *result)
{
   struct mem_thread_arg *args =
      (struct mem_thread_arg *)calloc((size_t)nthreads, sizeof(*args));
   pthread_t *threads = NULL;
   int len = (int)bytes;
   int err = 0;
   int created = 0;
   double start;

   if (!args)
      return -ENOMEM;
   if (nthreads > 1) {
      threads = (pthread_t *)calloc((size_t)nthreads, sizeof(*threads));
      if (!threads) {
         free(args);
         return -ENOMEM;
      }
   }

   for (int i = 0; i < nthreads; ++i) {
      args[i].handle = handles[i];
      args[i].scenario = scenario;
      args[i].src = srcs[i];
      args[i].dst = dsts[i];
      args[i].len = len;
      args[i].repeats = repeats;
   }

   start = now_ms();
   if (nthreads == 1) {
      mem_thread_main(&args[0]);
   } else {
      for (int i = 0; i < nthreads; ++i) {
         err = pthread_create(&threads[i], NULL, mem_thread_main, &args[i]);
         if (err)
            break;
         ++created;
      }
      for (int i = 0; i < created; ++i)
         pthread_join(threads[i], NULL);
   }
   result->elapsed_ms = now_ms() - start;

   result->count = (uint64_t)repeats * (uint64_t)nthreads;
   result->cycles = 0;
   result->checksum = 0xcbf29ce484222325ULL;

   if (!err) {
      for (int i = 0; i < nthreads; ++i) {
         if (args[i].err) {
            err = args[i].err;
            break;
         }
         if (args[i].cycles > result->cycles)
            result->cycles = args[i].cycles;
         result->checksum = mix_checksum(result->checksum, args[i].checksum,
                                         (uint64_t)i);
      }
   }

   free(threads);
   free(args);
   return err;
}

static int run_mem(const remote_handle64 *handles, const struct options *opt,
                   const struct mem_scenario *scenario, uint8_t **srcs,
                   uint8_t **dsts, size_t bytes)
{
   int repeats = 1;
   int err;
   struct bench_result result = {0};

   for (;;) {
      err = measure_mem_threads(handles, opt->threads, scenario, srcs, dsts,
                                bytes, repeats, &result);
      if (err)
         return err;

      if (result.elapsed_ms >= (double)opt->min_ms || repeats > (1 << 24))
         break;
      repeats *= 2;
   }

   double transferred = (double)bytes * (double)result.count *
                        scenario->bytes_scale;
   double score = transferred / (result.elapsed_ms / 1000.0) / 1.0e9;
   double bytes_per_cycle = result.cycles ? transferred / (double)result.cycles : 0.0;

   print_row(scenario->name, opt->threads, result.count, result.elapsed_ms,
             result.cycles, bytes_per_cycle, score, "GB/s", result.checksum);
   return 0;
}

static bool any_mem_scenario_enabled(const struct options *opt)
{
   for (unsigned i = 0; i < ARRAY_SIZE(mem_scenarios); ++i) {
      if (scenario_enabled(opt, mem_scenarios[i].name))
         return true;
   }

   return false;
}

static bool any_hmx_scenario_enabled(const struct options *opt)
{
   for (unsigned i = 0; i < ARRAY_SIZE(hmx_scenarios); ++i) {
      if (hmx_scenario_enabled(opt, &hmx_scenarios[i], 0))
         return true;
   }

   return false;
}

static size_t max_enabled_hmx_output_bytes(const struct options *opt, int arch)
{
   size_t bytes = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(hmx_scenarios); ++i) {
      if (!hmx_scenario_enabled(opt, &hmx_scenarios[i], arch))
         continue;
      if (hmx_scenarios[i].output_bytes > bytes)
         bytes = hmx_scenarios[i].output_bytes;
   }

   return bytes;
}

static size_t max_enabled_hmx_activation_bytes(const struct options *opt,
                                               int arch)
{
   size_t bytes = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(hmx_scenarios); ++i) {
      if (!hmx_scenario_enabled(opt, &hmx_scenarios[i], arch))
         continue;
      if (hmx_scenarios[i].activation_bytes > bytes)
         bytes = hmx_scenarios[i].activation_bytes;
   }

   return bytes;
}

static size_t max_enabled_hmx_weight_bytes(const struct options *opt, int arch)
{
   size_t bytes = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(hmx_scenarios); ++i) {
      if (!hmx_scenario_enabled(opt, &hmx_scenarios[i], arch))
         continue;
      if (hmx_scenarios[i].weight_bytes > bytes)
         bytes = hmx_scenarios[i].weight_bytes;
   }

   return bytes;
}

int main(int argc, char **argv)
{
   struct options opt;
   char *uri = NULL;
   remote_handle64 *handles = NULL;
   int err = 0;
   int arch = 0;
   int hvx_bytes = 0;
   uint32_t arch_capability = 68;
   uint64_t timer_overhead = 0;
   uint64_t current_cycles = 0;
   uint32_t hvx_64b = 0;
   uint32_t hvx_128b = 0;
   uint32_t unsigned_pd_support = UINT32_MAX;
   uint32_t vtcm_page = 0;
   uint32_t vtcm_count = 0;
   uint32_t hmx_depth = 0;
   uint32_t hmx_spatial = 0;
   int power_applied_mask = 0;
   const char *skel_suffix;
   uint8_t **srcs = NULL;
   uint8_t **dsts = NULL;
   uint8_t *hmx_activation = NULL;
   uint8_t *hmx_weight = NULL;
   uint8_t *hmx_output = NULL;
   size_t bytes;
   bool need_mem;
   bool need_hmx;

   parse_options(argc, argv, &opt);
   need_mem = any_mem_scenario_enabled(&opt);
   need_hmx = any_hmx_scenario_enabled(&opt);
   add_executable_dir_to_dsp_paths();

   bytes = opt.size_mib * 1024u * 1024u;
   bytes &= ~(size_t)127u;
   if (bytes > (size_t)INT32_MAX) {
      fprintf(stderr, "--size-mib is too large for this QAIC v1 interface\n");
      return 2;
   }

   reset_domain_if_requested(opt.domain, opt.reset);

   (void)query_dsp_capability(opt.domain, UNSIGNED_PD_SUPPORT,
                              &unsigned_pd_support);

   if (opt.unsigned_pd != UNSIGNED_PD_OFF) {
      err = enable_unsigned_pd(opt.domain, 1);
      if (err) {
         if (opt.unsigned_pd == UNSIGNED_PD_REQUIRED) {
            if (unsigned_pd_support != UINT32_MAX)
               fprintf(stderr,
                       "unsigned PD enable failed: 0x%x"
                       " (UNSIGNED_PD_SUPPORT=%u)\n",
                       err, unsigned_pd_support);
            else
               fprintf(stderr, "unsigned PD enable failed: 0x%x\n", err);
            return 1;
         }
         if (unsigned_pd_support != UINT32_MAX)
            fprintf(stderr,
                    "warning: unsigned PD enable failed: 0x%x"
                    " (UNSIGNED_PD_SUPPORT=%u); continuing\n",
                    err, unsigned_pd_support);
         else
            fprintf(stderr,
                    "warning: unsigned PD enable failed: 0x%x; continuing\n",
                    err);
      }
   }

   (void)query_dsp_capability(opt.domain, ARCH_VER, &arch_capability);
   (void)query_dsp_capability(opt.domain, HVX_SUPPORT_64B, &hvx_64b);
   (void)query_dsp_capability(opt.domain, HVX_SUPPORT_128B, &hvx_128b);
   resolve_thread_count(&opt, hvx_64b, hvx_128b);

   err = make_uri(opt.domain, arch_capability, &uri);
   if (err) {
      if (err != -ENOTSUP)
         fprintf(stderr, "make_uri failed: %d\n", err);
      return 1;
   }
   skel_suffix = skel_arch_suffix(arch_capability);
   if (!skel_suffix)
      skel_suffix = "custom";

   handles = (remote_handle64 *)calloc((size_t)opt.threads, sizeof(*handles));
   if (!handles) {
      err = -ENOMEM;
      goto out;
   }

   for (int i = 0; i < opt.threads; ++i) {
      err = cdsp_peak_open(uri, &handles[i]);
      if (err) {
         fprintf(stderr, "cdsp_peak_open thread %d failed: 0x%x (uri=%s)\n",
                 i, err, uri);
#ifdef _WIN32
         if (err == AEE_EUNABLETOLOAD || (uint32_t)err == 0x80000406u)
            fprintf(stderr,
                    "on Windows this usually means the DSP skel was not found "
                    "or its catalog signature was not accepted\n");
#endif
         goto out;
      }
   }

   err = apply_power_config(handles, opt.threads, opt.power_mode,
                            &power_applied_mask);
   if (err)
      goto out;

   err = cdsp_peak_get_info(handles[0], &arch, &hvx_bytes, &timer_overhead,
                            &current_cycles);
   if (err) {
      fprintf(stderr, "cdsp_peak_get_info failed: 0x%x\n", err);
      goto out;
   }

   (void)query_dsp_capability(opt.domain, VTCM_PAGE, &vtcm_page);
   (void)query_dsp_capability(opt.domain, VTCM_COUNT, &vtcm_count);
   (void)query_dsp_capability(opt.domain, HMX_SUPPORT_DEPTH, &hmx_depth);
   (void)query_dsp_capability(opt.domain, HMX_SUPPORT_SPATIAL, &hmx_spatial);

   printf("cdsp_peak domain=%s arch=v%d skel=%s hvx=%dB hvx64=%u hvx128=%u"
          " timer_overhead=%" PRIu64
          " cycles threads=%d buffer=%zu MiB/thread vtcm_page=%u"
          " vtcm_count=%u hmx_depth=%u hmx_spatial=%u power=%s"
          " power_mask=0x%x\n",
          domain_name(opt.domain), arch, skel_suffix, hvx_bytes, hvx_64b,
          hvx_128b, timer_overhead, opt.threads, bytes / (1024u * 1024u),
          vtcm_page, vtcm_count, hmx_depth, hmx_spatial,
          power_mode_name(opt.power_mode), power_applied_mask);

   if (need_mem) {
      srcs = (uint8_t **)calloc((size_t)opt.threads, sizeof(*srcs));
      dsts = (uint8_t **)calloc((size_t)opt.threads, sizeof(*dsts));
      if (!srcs || !dsts) {
         err = -ENOMEM;
         goto out;
      }

      for (int i = 0; i < opt.threads; ++i) {
         srcs[i] = (uint8_t *)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM,
                                           RPCMEM_DEFAULT_FLAGS, bytes);
         dsts[i] = (uint8_t *)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM,
                                           RPCMEM_DEFAULT_FLAGS, bytes);
         if (!srcs[i] || !dsts[i]) {
            fprintf(stderr, "rpcmem_alloc thread %d failed for %zu bytes\n",
                    i, bytes);
            err = -ENOMEM;
            goto out;
         }

         fill_source(srcs[i], bytes);
         memset(dsts[i], 0, bytes);
      }
   }

   if (need_hmx && opt.threads > 1)
      fprintf(stderr, "warning: HMX scenarios use one FastRPC handle; --threads is ignored for HMX rows\n");

   print_header();

   if (scenario_enabled(&opt, "rpc-null")) {
      err = run_rpc_null(handles, &opt);
      if (err)
         goto out;
   }

   unsigned completed_groups = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(compute_scenarios); ++i) {
      if (!scenario_enabled(&opt, compute_scenarios[i].name))
         continue;

      if (compute_scenarios[i].iteration_group != ITERATION_GROUP_NONE) {
         unsigned group_bit = 1u << compute_scenarios[i].iteration_group;

         if (completed_groups & group_bit)
            continue;
         err = run_compute_group(handles, &opt,
                                 compute_scenarios[i].iteration_group);
         completed_groups |= group_bit;
      } else {
         err = run_compute(handles, &opt, &compute_scenarios[i]);
      }
      if (err) {
         fprintf(stderr, "%s failed: 0x%x\n", compute_scenarios[i].name, err);
         goto out;
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(hmx_scenarios); ++i) {
      if (!hmx_scenario_enabled(&opt, &hmx_scenarios[i], arch))
         continue;
      if (!hmx_activation) {
         size_t hmx_activation_bytes =
            max_enabled_hmx_activation_bytes(&opt, arch);
         size_t hmx_weight_bytes = max_enabled_hmx_weight_bytes(&opt, arch);
         size_t hmx_output_bytes = max_enabled_hmx_output_bytes(&opt, arch);
         hmx_activation = (uint8_t *)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM,
                                                  RPCMEM_DEFAULT_FLAGS,
                                                  hmx_activation_bytes);
         hmx_weight = (uint8_t *)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM,
                                              RPCMEM_DEFAULT_FLAGS,
                                              hmx_weight_bytes);
         hmx_output = (uint8_t *)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM,
                                              RPCMEM_DEFAULT_FLAGS,
                                              hmx_output_bytes);
         if (!hmx_activation || !hmx_weight || !hmx_output) {
            fprintf(stderr, "rpcmem_alloc failed for HMX probe buffers\n");
            err = -ENOMEM;
            goto out;
         }
      }

      err = run_hmx_int8(handles[0], &opt, &hmx_scenarios[i], hmx_activation,
                         hmx_weight, hmx_output);
      if (err) {
         fprintf(stderr, "%s failed: 0x%x\n", hmx_scenarios[i].name, err);
         goto out;
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(mem_scenarios); ++i) {
      if (!scenario_enabled(&opt, mem_scenarios[i].name))
         continue;
      err = run_mem(handles, &opt, &mem_scenarios[i], srcs, dsts, bytes);
      if (err) {
         fprintf(stderr, "%s failed: 0x%x\n", mem_scenarios[i].name, err);
         goto out;
      }
   }

out:
   if (srcs) {
      for (int i = 0; i < opt.threads; ++i) {
         if (srcs[i])
            rpcmem_free(srcs[i]);
      }
   }
   if (dsts) {
      for (int i = 0; i < opt.threads; ++i) {
         if (dsts[i])
            rpcmem_free(dsts[i]);
      }
   }
   if (hmx_activation)
      rpcmem_free(hmx_activation);
   if (hmx_weight)
      rpcmem_free(hmx_weight);
   if (hmx_output)
      rpcmem_free(hmx_output);
   if (handles) {
      for (int i = 0; i < opt.threads; ++i) {
         if (handles[i])
            cdsp_peak_close(handles[i]);
      }
   }
   free(srcs);
   free(dsts);
   free(handles);
   free(uri);

   return err ? 1 : 0;
}
