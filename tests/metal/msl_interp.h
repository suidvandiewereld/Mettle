#ifndef MSL_INTERP_H
#define MSL_INTERP_H

#include <stddef.h>
#include <stdint.h>

typedef struct MslProgram MslProgram;

MslProgram *msl_program_load(const char *source, size_t length, char *error, size_t error_size);
void msl_program_free(MslProgram *program);
uint64_t msl_device_register(MslProgram *program, void *host, size_t size);

typedef struct {
  const char *kernel;
  uint32_t grid[3];
  uint32_t block[3];
  const void *args;
  size_t args_size;
  size_t threadgroup_bytes;
  uint32_t simd_width;
  int spurious_cas;
  void (*log)(void *user, const char *text);
  void *log_user;
} MslDispatch;

int msl_dispatch(MslProgram *program, const MslDispatch *dispatch, char *error, size_t error_size);
int msl_selftest(char *error, size_t error_size);

#endif
