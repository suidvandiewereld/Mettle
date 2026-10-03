#include "msl_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  char *data;
  long size;
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  data = (char *)malloc((size_t)size + 1);
  if (data == NULL) {
    fclose(file);
    return NULL;
  }
  if (fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  data[size] = 0;
  fclose(file);
  *length = (size_t)size;
  return data;
}

static MslProgram *load_program(const char *path, char *error, size_t error_size) {
  size_t length = 0;
  char *source = read_file(path, &length);
  MslProgram *program;
  if (source == NULL) {
    snprintf(error, error_size, "cannot read file");
    return NULL;
  }
  program = msl_program_load(source, length, error, error_size);
  free(source);
  return program;
}

static int check_files(int count, char **paths) {
  int failures = 0;
  int i;
  for (i = 0; i < count; i++) {
    char error[1024];
    MslProgram *program = load_program(paths[i], error, sizeof error);
    if (program == NULL) {
      printf("FAIL %s: %s\n", paths[i], error);
      failures++;
      continue;
    }
    printf("ok %s\n", paths[i]);
    msl_program_free(program);
  }
  return failures == 0 ? 0 : 1;
}

static int run_selftest(void) {
  char error[2048];
  if (!msl_selftest(error, sizeof error)) {
    printf("selftest FAILED: %s\n", error);
    return 1;
  }
  printf("%s\n", error);
  return 0;
}

static int run_kernel(const char *path, const char *kernel) {
  char error[1024];
  MslProgram *program = load_program(path, error, sizeof error);
  MslDispatch dispatch;
  int ok;
  if (program == NULL) {
    printf("FAIL %s: %s\n", path, error);
    return 1;
  }
  memset(&dispatch, 0, sizeof dispatch);
  dispatch.kernel = kernel;
  dispatch.grid[0] = 1;
  dispatch.grid[1] = 1;
  dispatch.grid[2] = 1;
  dispatch.block[0] = 32;
  dispatch.block[1] = 1;
  dispatch.block[2] = 1;
  ok = msl_dispatch(program, &dispatch, error, sizeof error);
  msl_program_free(program);
  if (!ok) {
    printf("FAIL %s: %s\n", path, error);
    return 1;
  }
  printf("ok %s %s\n", path, kernel);
  return 0;
}

static void usage(void) {
  printf("usage: msl_run --check FILE...\n");
  printf("       msl_run --selftest\n");
  printf("       msl_run --run FILE KERNEL\n");
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
    return check_files(argc - 2, argv + 2);
  }
  if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
    return run_selftest();
  }
  if (argc == 4 && strcmp(argv[1], "--run") == 0) {
    return run_kernel(argv[2], argv[3]);
  }
  usage();
  return 2;
}
