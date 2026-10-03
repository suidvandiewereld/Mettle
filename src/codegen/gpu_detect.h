#ifndef GPU_DETECT_H
#define GPU_DETECT_H

#include <stddef.h>

#define GPU_DETECT_MAX_DEVICES 8

typedef struct GpuDetectDevice {
  char name[128];
  int compute_major;
  int compute_minor;
  int multiprocessor_count;
  int warp_size;
  int max_threads_per_block;
  int max_shared_memory_per_block;
  int clock_khz;
  int integrated;
  long long total_memory;
} GpuDetectDevice;

typedef struct GpuDetectResult {
  int available;
  int device_count;
  int driver_version;
  const char *source;
  GpuDetectDevice devices[GPU_DETECT_MAX_DEVICES];
} GpuDetectResult;

const GpuDetectResult *gpu_detect_local(void);

typedef struct GpuMetalDetect {
  int available;
  int metal3;
  int unified_memory;
  int threadgroup_memory;
  long long working_set;
  char name[128];
} GpuMetalDetect;

const GpuMetalDetect *gpu_detect_metal(void);

int gpu_detect_ptx_target(int device, char *out, size_t out_size);

const char *gpu_detect_ptxas_targets(void);

int gpu_detect_ptxas_supports(const char *target);

const char *gpu_detect_ptxas_version(void);

int gpu_detect_ptx_isa(int *major, int *minor);

#endif
