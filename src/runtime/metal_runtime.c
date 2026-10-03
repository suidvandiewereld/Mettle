#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "metal_provider.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define METAL_RT_MAX_KERNELS 256

static MettleMetal *g_metal;
static char g_error[2048];
static char *g_kernels[METAL_RT_MAX_KERNELS];
static size_t g_kernel_count;
static int g_version_major = 3;
static int g_version_minor = 2;

static void rt_error(const char *text) {
  snprintf(g_error, sizeof(g_error), "%s", text);
}

static void rt_forget_kernels(void) {
  for (size_t i = 0; i < g_kernel_count; i++) {
    free(g_kernels[i]);
    g_kernels[i] = NULL;
  }
  g_kernel_count = 0;
}

static void rt_read_version(void) {
  const char *text = getenv("METTLE_METAL_VERSION");
  int major = 0;
  int minor = 0;
  if (text && sscanf(text, "%d.%d", &major, &minor) == 2 && major >= 3) {
    g_version_major = major;
    g_version_minor = minor;
  }
}

const char *mettle_metal_rt_error(void) { return g_error; }

void mettle_metal_rt_set_version(int32_t major, int32_t minor) {
  g_version_major = major;
  g_version_minor = minor;
}

int32_t mettle_metal_rt_init(void) {
  if (g_metal) {
    return 1;
  }
  rt_read_version();
  g_metal = mettle_metal_open(g_error, sizeof(g_error));
  return g_metal ? 1 : 0;
}

void mettle_metal_rt_shutdown(void) {
  rt_forget_kernels();
  mettle_metal_close(g_metal);
  g_metal = NULL;
}

const char *mettle_metal_rt_device_name(void) {
  return g_metal ? mettle_metal_device_name(g_metal) : "";
}

int64_t mettle_metal_rt_property(int32_t which) {
  return g_metal ? mettle_metal_property(g_metal, which) : 0;
}

int32_t mettle_metal_rt_load_source(const char *text) {
  if (!text) {
    rt_error("no Metal source was given");
    return 0;
  }
  if (!mettle_metal_rt_init()) {
    return 0;
  }
  rt_forget_kernels();
  return mettle_metal_load(g_metal, text, strlen(text), g_version_major,
                           g_version_minor, g_error, sizeof(g_error))
             ? 1
             : 0;
}

int32_t mettle_metal_rt_load_file(const char *path) {
  if (!path) {
    rt_error("no Metal module path was given");
    return 0;
  }
  if (!mettle_metal_rt_init()) {
    return 0;
  }
  rt_forget_kernels();
  return mettle_metal_load_file(g_metal, path, g_version_major,
                                g_version_minor, g_error, sizeof(g_error))
             ? 1
             : 0;
}

int64_t mettle_metal_rt_kernel(const char *name) {
  char *copy;
  size_t length;
  if (!name || !g_metal) {
    rt_error("no Metal module is loaded");
    return 0;
  }
  for (size_t i = 0; i < g_kernel_count; i++) {
    if (strcmp(g_kernels[i], name) == 0) {
      return (int64_t)i + 1;
    }
  }
  if (g_kernel_count == METAL_RT_MAX_KERNELS) {
    rt_error("more than 256 distinct Metal kernels were looked up");
    return 0;
  }
  if (!mettle_metal_prepare(g_metal, name, g_error, sizeof(g_error))) {
    return 0;
  }
  length = strlen(name) + 1;
  copy = (char *)malloc(length);
  if (!copy) {
    rt_error("out of memory recording a Metal kernel");
    return 0;
  }
  memcpy(copy, name, length);
  g_kernels[g_kernel_count++] = copy;
  return (int64_t)g_kernel_count;
}

const char *mettle_metal_rt_kernel_name(int64_t handle) {
  if (handle < 1 || (uint64_t)handle > g_kernel_count) {
    return "an unknown kernel handle";
  }
  return g_kernels[handle - 1];
}

int64_t mettle_metal_rt_alloc(int64_t bytes) {
  uint64_t address;
  if (bytes < 0 || !mettle_metal_rt_init()) {
    return 0;
  }
  address = mettle_metal_alloc(g_metal, (size_t)bytes);
  if (!address) {
    snprintf(g_error, sizeof(g_error),
             "Metal could not allocate %lld bytes", (long long)bytes);
  }
  return (int64_t)address;
}

uint8_t *mettle_metal_rt_host(int64_t address) {
  return g_metal ? (uint8_t *)mettle_metal_host(g_metal, (uint64_t)address)
                 : NULL;
}

int32_t mettle_metal_rt_free(int64_t address) {
  if (!g_metal || !mettle_metal_host(g_metal, (uint64_t)address)) {
    return 1;
  }
  mettle_metal_free(g_metal, (uint64_t)address);
  return 0;
}

uint8_t *mettle_metal_rt_alloc_mapped(int64_t bytes, int64_t *device_out) {
  int64_t address = mettle_metal_rt_alloc(bytes);
  if (device_out) {
    *device_out = address;
  }
  return address ? mettle_metal_rt_host(address) : NULL;
}

int32_t mettle_metal_rt_free_mapped(uint8_t *host) {
  uint64_t address = g_metal ? mettle_metal_address_of(g_metal, host) : 0;
  if (!address) {
    return 1;
  }
  return mettle_metal_rt_free((int64_t)address);
}

static int rt_span_ok(int64_t address, int64_t bytes, const char *what) {
  size_t span;
  if (bytes < 0) {
    snprintf(g_error, sizeof(g_error), "%s of a negative byte count", what);
    return 0;
  }
  if (bytes == 0) {
    return 1;
  }
  span = g_metal ? mettle_metal_span(g_metal, (uint64_t)address) : 0;
  if ((uint64_t)bytes > span) {
    snprintf(g_error, sizeof(g_error),
             "%s of %lld bytes at 0x%llx runs past its Metal buffer", what,
             (long long)bytes, (unsigned long long)address);
    return 0;
  }
  return 1;
}

int32_t mettle_metal_rt_to_device(int64_t dst, const uint8_t *src,
                                  int64_t bytes) {
  if (!rt_span_ok(dst, bytes, "a copy to the GPU")) {
    return 1;
  }
  if (bytes) {
    memcpy(mettle_metal_rt_host(dst), src, (size_t)bytes);
  }
  return 0;
}

int32_t mettle_metal_rt_to_host(uint8_t *dst, int64_t src, int64_t bytes) {
  if (!rt_span_ok(src, bytes, "a copy from the GPU")) {
    return 1;
  }
  if (bytes) {
    memcpy(dst, mettle_metal_rt_host(src), (size_t)bytes);
  }
  return 0;
}

static size_t rt_align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static int rt_pack(const int64_t *params, int32_t nargs, unsigned char **block,
                   size_t *block_size) {
  size_t offset = 0;
  size_t max_align = 1;
  unsigned char *bytes;
  *block = NULL;
  *block_size = 0;
  if (nargs <= 0) {
    return 1;
  }
  if (!params) {
    rt_error("a launch with arguments passed no parameter table");
    return 0;
  }
  for (int32_t pass = 0; pass < 2; pass++) {
    offset = 0;
    for (int32_t i = 0; i < nargs; i++) {
      uint64_t word = (uint64_t)params[nargs + i];
      size_t size = (size_t)(word & 0xFFFFFFu);
      size_t alignment = (size_t)((word >> 24) & 0x7Fu);
      if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) ||
          (word >> 31) != 0 || params[i] == 0) {
        snprintf(g_error, sizeof(g_error),
                 "argument %d of the launch has no valid layout word; a "
                 "Metal launch needs nargs pointers followed by nargs "
                 "(alignment << 24 | size) words",
                 (int)i);
        free(*block);
        *block = NULL;
        return 0;
      }
      offset = rt_align_up(offset, alignment);
      if (pass == 1) {
        memcpy(*block + offset, (const void *)(uintptr_t)params[i], size);
      } else if (alignment > max_align) {
        max_align = alignment;
      }
      offset += size;
    }
    if (pass == 0) {
      *block_size = rt_align_up(offset, max_align);
      bytes = (unsigned char *)calloc(*block_size ? *block_size : 1, 1);
      if (!bytes) {
        rt_error("out of memory packing Metal launch arguments");
        return 0;
      }
      *block = bytes;
    }
  }
  return 1;
}

int32_t mettle_metal_rt_launch(int64_t kernel, int32_t gx, int32_t gy,
                               int32_t gz, int32_t bx, int32_t by, int32_t bz,
                               int32_t shared_bytes, int64_t stream,
                               const int64_t *params, int32_t nargs) {
  unsigned char *block = NULL;
  size_t block_size = 0;
  uint32_t grid[3];
  uint32_t threads[3];
  int ok;
  (void)stream;
  if (!g_metal || kernel < 1 || (uint64_t)kernel > g_kernel_count) {
    rt_error("the launch names no prepared Metal kernel");
    return 1;
  }
  if (gx <= 0 || gy <= 0 || gz <= 0 || bx <= 0 || by <= 0 || bz <= 0 ||
      shared_bytes < 0) {
    rt_error("grid, block and shared bytes must be positive");
    return 1;
  }
  if (!rt_pack(params, nargs, &block, &block_size)) {
    return 1;
  }
  grid[0] = (uint32_t)gx;
  grid[1] = (uint32_t)gy;
  grid[2] = (uint32_t)gz;
  threads[0] = (uint32_t)bx;
  threads[1] = (uint32_t)by;
  threads[2] = (uint32_t)bz;
  ok = mettle_metal_launch(g_metal, g_kernels[kernel - 1], grid, threads,
                           block, block_size, (size_t)shared_bytes, g_error,
                           sizeof(g_error));
  free(block);
  return ok ? 0 : 1;
}

void mettle_metal_rt_note(const char *text) {
  fputs(text ? text : "", stderr);
}

void mettle_metal_rt_fail(const char *first, const char *second,
                          const char *third) {
  fprintf(stderr, "%s%s%s\n", first ? first : "", second ? second : "",
          third ? third : "");
  fflush(stderr);
  exit(1);
}

void mettle_metal_rt_launch_checked(int64_t kernel, int32_t gx, int32_t gy,
                                    int32_t gz, int32_t bx, int32_t by,
                                    int32_t bz, int32_t shared_bytes,
                                    int64_t stream, const int64_t *params,
                                    int32_t nargs) {
  if (mettle_metal_rt_launch(kernel, gx, gy, gz, bx, by, bz, shared_bytes,
                             stream, params, nargs) == 0) {
    return;
  }
  fprintf(stderr,
          "mettle: GPU launch of %s failed with grid %dx%dx%d, block "
          "%dx%dx%d, %d shared bytes\n  Metal: %s\n",
          mettle_metal_rt_kernel_name(kernel), (int)gx, (int)gy, (int)gz,
          (int)bx, (int)by, (int)bz, (int)shared_bytes, g_error);
  fflush(stderr);
  exit(1);
}

double mettle_metal_rt_now_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
#endif
}
