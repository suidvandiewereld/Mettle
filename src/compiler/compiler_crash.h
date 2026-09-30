#ifndef COMPILER_CRASH_H
#define COMPILER_CRASH_H

#include <stdio.h>

typedef void (*MettleIceBackendDump)(const void *arg, FILE *output);

void mettle_compiler_crash_install(int argc, char **argv);

const char *mettle_compiler_ice_bundle_dir(void);

FILE *mettle_compiler_ice_bundle_open(const char *name);

void mettle_compiler_ice_set_backend_dump(MettleIceBackendDump dump,
                                          const void *arg);

void mettle_compiler_ice_capture_backend(void);

void mettle_compiler_ice_report(const char *reason, const char *detail);

void mettle_compiler_ice(const char *reason);

#endif
