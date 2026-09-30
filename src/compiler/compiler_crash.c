#include "compiler_crash.h"
#include "compiler_context.h"
#include "../runtime/crash_handler.h"
#include "../runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#define MAX_BACKTRACE_FRAMES 64
#define MAX_SYM_NAME_LEN 1024

#if defined(_WIN32) || defined(_WIN64)
#define METTLE_ICE_SEP '\\'
#else
#define METTLE_ICE_SEP '/'
#endif

static int g_compiler_crash_installed = 0;
static int g_compiler_in_ice_handler = 0;
static int g_compiler_argc = 0;
static char **g_compiler_argv = NULL;
static int g_ice_bundle_state = 0;
static char g_ice_bundle_dir[1024];
static MettleIceBackendDump g_ice_backend_dump = NULL;
static const void *g_ice_backend_arg = NULL;
#if defined(_WIN32) || defined(_WIN64)
static int g_sym_initialized = 0;
static CONTEXT *g_compiler_crash_context = NULL;
#endif

#if defined(_WIN32) || defined(_WIN64)
static void mettle_compiler_sym_init(void) {
  HANDLE process = GetCurrentProcess();

  if (g_sym_initialized) {
    return;
  }

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_INCLUDE_32BIT_MODULES);
  SymInitialize(process, NULL, TRUE);
  g_sym_initialized = 1;
}

static int mettle_compiler_frame_is_internal(const char *name) {
  if (!name || name[0] == '\0') {
    return 0;
  }

  return strstr(name, "mettle_compiler_write_backtrace") != NULL ||
         strstr(name, "mettle_compiler_write_frame") != NULL ||
         strstr(name, "mettle_compiler_ice_report") != NULL ||
         strstr(name, "mettle_compiler_unhandled_exception_filter") != NULL ||
         strstr(name, "CaptureStackBackTrace") != NULL ||
         strstr(name, "StackWalk64") != NULL ||
         strstr(name, "RtlCaptureStackBackTrace") != NULL ||
         strstr(name, "SymFunctionTableAccess64") != NULL ||
         strstr(name, "SymGetModuleBase64") != NULL;
}

static void mettle_compiler_write_module_offset(FILE *output, void *address) {
  HMODULE module = NULL;
  char module_path[MAX_PATH];
  const char *basename = NULL;
  DWORD64 module_base = 0;

  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)address, &module) ||
      !module) {
    fprintf(output, "%p", address);
    return;
  }

  if (GetModuleFileNameA(module, module_path, sizeof(module_path)) == 0) {
    fprintf(output, "%p", address);
    return;
  }

  basename = strrchr(module_path, '\\');
  basename = basename ? basename + 1 : module_path;
  module_base = (DWORD64)(uintptr_t)module;
  fprintf(output, "%s+0x%llX", basename,
          (unsigned long long)((DWORD64)(uintptr_t)address - module_base));
}

static void mettle_compiler_write_frame(FILE *output, HANDLE process,
                                        void *frame_address, size_t index) {
  DWORD64 address = (DWORD64)(uintptr_t)frame_address;
  DWORD64 lookup_address = address;
  char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME_LEN * sizeof(TCHAR)];
  SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
  IMAGEHLP_LINE64 line = {0};
  DWORD64 symbol_displacement = 0;
  DWORD line_displacement = 0;
  int has_symbol = 0;

  if (address != 0) {
    lookup_address = address - 1;
  }

  symbol->MaxNameLen = MAX_SYM_NAME_LEN;
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  line.SizeOfStruct = sizeof(line);

  fprintf(output, "  #%zu ", index);

  if (SymFromAddr(process, lookup_address, &symbol_displacement, symbol)) {
    if (!mettle_compiler_frame_is_internal(symbol->Name)) {
      has_symbol = 1;
      fprintf(output, "%s", symbol->Name);
      if (symbol_displacement != 0) {
        fprintf(output, "+0x%llX", (unsigned long long)symbol_displacement);
      }
    }
  }

  if (!has_symbol) {
    mettle_compiler_write_module_offset(output, frame_address);
  }

  if (SymGetLineFromAddr64(process, lookup_address, &line_displacement, &line) &&
      line.FileName && line.LineNumber > 0) {
    fprintf(output, " at %s:%lu", line.FileName, line.LineNumber);
  }

  fprintf(output, " (%p)\n", frame_address);
}

static void mettle_compiler_write_backtrace_with_context(FILE *output,
                                                         CONTEXT *context) {
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  size_t frame_index = 0;

  if (!output) {
    return;
  }

  fprintf(output, "\nCompiler backtrace:\n");
  mettle_compiler_sym_init();

  if (context) {
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrPC.Offset = context->Rip;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;

    for (size_t i = 0; i < MAX_BACKTRACE_FRAMES; i++) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                       context, NULL, SymFunctionTableAccess64,
                       SymGetModuleBase64, NULL)) {
        break;
      }
      if (frame.AddrPC.Offset == 0) {
        break;
      }

      mettle_compiler_write_frame(
          output, process, (void *)(uintptr_t)frame.AddrPC.Offset, frame_index);
      frame_index++;
    }
#else
    (void)thread;
    (void)frame_index;
#endif
  } else {
    void *frames[MAX_BACKTRACE_FRAMES];
    USHORT frame_count =
        CaptureStackBackTrace(0, MAX_BACKTRACE_FRAMES, frames, NULL);

    for (USHORT i = 0; i < frame_count; i++) {
      char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME_LEN * sizeof(TCHAR)];
      SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
      DWORD64 displacement = 0;
      int skip = 0;

      symbol->MaxNameLen = MAX_SYM_NAME_LEN;
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      if (SymFromAddr(process, (DWORD64)(uintptr_t)frames[i] - 1, &displacement,
                      symbol) &&
          mettle_compiler_frame_is_internal(symbol->Name)) {
        skip = 1;
      }

      if (skip) {
        continue;
      }

      mettle_compiler_write_frame(output, process, frames[i], frame_index);
      frame_index++;
    }
  }
}
#else
static void mettle_compiler_write_frame(FILE *output, void *frame_address,
                                         size_t index) {
  fprintf(output, "  #%zu %p\n", index, frame_address);
}

static void mettle_compiler_write_backtrace_with_context(FILE *output,
                                                          void *context) {
  (void)context;
  if (!output) {
    return;
  }
  fprintf(output, "\nCompiler backtrace:\n");

  void **frame = (void **)__builtin_frame_address(0);
  for (size_t index = 0; frame && index < MAX_BACKTRACE_FRAMES; index++) {
    void **next = (void **)frame[0];
    void *return_address = frame[1];
    if (!return_address) {
      break;
    }
    if (index >= 2) {
      mettle_compiler_write_frame(output, return_address, index - 2);
    }
    if (!next || next <= frame || (uintptr_t)next - (uintptr_t)frame >
                                      16u * 1024u * 1024u ||
        ((uintptr_t)next & (sizeof(void *) - 1u)) != 0) {
      break;
    }
    frame = next;
  }
}
#endif

static void mettle_compiler_write_backtrace(FILE *output) {
#if defined(_WIN32) || defined(_WIN64)
  mettle_compiler_write_backtrace_with_context(output, g_compiler_crash_context);
#else
  mettle_compiler_write_backtrace_with_context(output, NULL);
#endif
}

static void mettle_compiler_write_backtrace_copy(FILE *output) {
#if defined(_WIN32) || defined(_WIN64)
  if (g_compiler_crash_context) {
    CONTEXT copy = *g_compiler_crash_context;
    mettle_compiler_write_backtrace_with_context(output, &copy);
    return;
  }
#endif
  mettle_compiler_write_backtrace_with_context(output, NULL);
}

static const char *mettle_ice_base_dir(void) {
  static const char *const names[] = {"METTLE_ICE_DIR", "TEMP", "TMP",
                                      "TMPDIR"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char *value = getenv(names[i]);
    if (value && value[0] != '\0') {
      return value;
    }
  }
#if defined(_WIN32) || defined(_WIN64)
  return ".";
#else
  return "/tmp";
#endif
}

static const char *mettle_ice_basename(const char *path) {
  const char *start = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      start = p + 1;
    }
  }
  return start;
}

const char *mettle_compiler_ice_bundle_dir(void) {
  const char *input = mettle_compiler_ctx()->input_filename;
  const char *base = NULL;
  char stem[128];
  size_t n = 0;

  if (g_ice_bundle_state != 0) {
    return g_ice_bundle_state > 0 ? g_ice_bundle_dir : NULL;
  }
  g_ice_bundle_state = -1;
  if (getenv("METTLE_NO_ICE_BUNDLE")) {
    return NULL;
  }
  base = mettle_ice_base_dir();
  if (!mettle_path_is_directory(base)) {
    (void)mettle_make_directory(base);
  }
  if (input) {
    const char *name = mettle_ice_basename(input);
    while (name[n] && name[n] != '.' && n + 1 < sizeof(stem)) {
      stem[n] = name[n];
      n++;
    }
  }
  stem[n] = '\0';
  for (int k = 1; k < 10000; k++) {
    snprintf(g_ice_bundle_dir, sizeof(g_ice_bundle_dir), "%s%c%s-ice-%d", base,
             METTLE_ICE_SEP, n ? stem : "mettle", k);
    if (mettle_path_exists(g_ice_bundle_dir)) {
      continue;
    }
    if (mettle_make_directory(g_ice_bundle_dir) != 0) {
      return NULL;
    }
    g_ice_bundle_state = 1;
    return g_ice_bundle_dir;
  }
  return NULL;
}

FILE *mettle_compiler_ice_bundle_open(const char *name) {
  char path[1200];
  const char *dir = mettle_compiler_ice_bundle_dir();
  if (!dir || !name) {
    return NULL;
  }
  snprintf(path, sizeof(path), "%s%c%s", dir, METTLE_ICE_SEP, name);
  return fopen(path, "wb");
}

void mettle_compiler_ice_set_backend_dump(MettleIceBackendDump dump,
                                          const void *arg) {
  g_ice_backend_dump = dump;
  g_ice_backend_arg = arg;
}

void mettle_compiler_ice_capture_backend(void) {
  MettleIceBackendDump dump = g_ice_backend_dump;
  const void *arg = g_ice_backend_arg;
  FILE *output = NULL;

  g_ice_backend_dump = NULL;
  g_ice_backend_arg = NULL;
  if (!dump) {
    return;
  }
  output = mettle_compiler_ice_bundle_open("mir.txt");
  if (!output) {
    return;
  }
  dump(arg, output);
  fclose(output);
}

static void mettle_ice_write_command(FILE *output) {
  char path[1024];
  char *environment = NULL;

  fprintf(output, "compiler: %s\n",
          mettle_executable_path(path, sizeof(path)) > 0 ? path : "?");
  fprintf(output, "directory: %s\n",
          mettle_getcwd(path, (int)sizeof(path)) == 0 ? path : "?");
  fprintf(output, "command:");
  for (int i = 0; i < g_compiler_argc; i++) {
    const char *arg = g_compiler_argv[i] ? g_compiler_argv[i] : "";
    fprintf(output, (arg[0] == '\0' || strpbrk(arg, " \t")) ? " \"%s\"" : " %s",
            arg);
  }
  fprintf(output, "\nenvironment:\n");
  environment = (char *)malloc(65536);
  if (environment) {
    (void)mettle_environment_write(environment, 65536, "METTLE_");
    fputs(environment, output);
    free(environment);
  }
}

static void mettle_ice_copy_input(const char *input) {
  char buffer[8192];
  FILE *source = NULL;
  FILE *copy = NULL;
  size_t n = 0;

  if (!input) {
    return;
  }
  source = fopen(input, "rb");
  if (!source) {
    return;
  }
  copy = mettle_compiler_ice_bundle_open(mettle_ice_basename(input));
  if (copy) {
    while ((n = fread(buffer, 1, sizeof(buffer), source)) > 0) {
      fwrite(buffer, 1, n, copy);
    }
    fclose(copy);
  }
  fclose(source);
}

static int mettle_ice_bundle_begin(const char *reason, const char *detail) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  FILE *output = mettle_compiler_ice_bundle_open("report.txt");

  if (!output) {
    return 0;
  }
  mettle_compiler_ctx_write_report(output, reason, detail);
  mettle_compiler_write_backtrace_copy(output);
  fclose(output);
  output = mettle_compiler_ice_bundle_open("command.txt");
  if (output) {
    mettle_ice_write_command(output);
    fclose(output);
  }
  mettle_ice_copy_input(ctx->input_filename);
  fprintf(stderr, "\nReproduction bundle: %s\n", g_ice_bundle_dir);
  return 1;
}

static void mettle_ice_bundle_finish(void) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  FILE *output = NULL;

  mettle_compiler_ice_capture_backend();
  if (!ctx->ir_program) {
    return;
  }
  output = mettle_compiler_ice_bundle_open("ir.txt");
  if (output) {
    (void)ir_program_dump(ctx->ir_program, output);
    fclose(output);
  }
}

void mettle_compiler_ice_report(const char *reason, const char *detail) {
  int bundle = 0;

  if (g_compiler_in_ice_handler) {
    fprintf(stderr, "Mettle internal compiler error (recursive)\n");
    return;
  }

  g_compiler_in_ice_handler = 1;
  mettle_compiler_ctx_write_snapshot();
  mettle_compiler_ctx_write_report(stderr, reason, detail);
  bundle = mettle_ice_bundle_begin(reason, detail);
  mettle_compiler_write_backtrace(stderr);
  if (bundle) {
    mettle_ice_bundle_finish();
  }
  g_compiler_in_ice_handler = 0;
}

void mettle_compiler_ice(const char *reason) {
  mettle_compiler_ice_report(reason, NULL);
  abort();
}

#if defined(_WIN32) || defined(_WIN64)
static LONG WINAPI mettle_compiler_unhandled_exception_filter(
    EXCEPTION_POINTERS *info) {
  char detail[64];

  if (g_compiler_in_ice_handler) {
    ExitProcess(3);
  }

  if (!info || !info->ExceptionRecord) {
    g_compiler_crash_context = info ? info->ContextRecord : NULL;
    mettle_compiler_ice_report("fatal exception", NULL);
    g_compiler_crash_context = NULL;
    ExitProcess(3);
  }

  snprintf(detail, sizeof(detail), "0x%08lX",
           (unsigned long)info->ExceptionRecord->ExceptionCode);
  g_compiler_crash_context = info->ContextRecord;
  mettle_compiler_ice_report(
      mettle_crash_exception_name(info->ExceptionRecord->ExceptionCode),
      detail);
  g_compiler_crash_context = NULL;
  ExitProcess(3);
}
#else
static void mettle_compiler_signal_handler(int signo, siginfo_t *info,
                                           void *ucontext_raw) {
  char detail[64];
  const char *reason = "fatal signal";

  (void)ucontext_raw;

  if (g_compiler_in_ice_handler) {
    _exit(128 + signo);
  }

  switch (signo) {
  case SIGSEGV:
    reason = "segmentation fault";
    break;
  case SIGBUS:
    reason = "bus error";
    break;
  case SIGFPE:
    reason = "floating-point exception";
    break;
  case SIGILL:
    reason = "illegal instruction";
    break;
  case SIGABRT:
    reason = "abort";
    break;
  default:
    break;
  }

  if (info && info->si_addr) {
    snprintf(detail, sizeof(detail), "at %p", info->si_addr);
    mettle_compiler_ice_report(reason, detail);
  } else {
    mettle_compiler_ice_report(reason, NULL);
  }
  _exit(128 + signo);
}
#endif

void mettle_compiler_crash_install(int argc, char **argv) {
  if (g_compiler_crash_installed) {
    return;
  }
  g_compiler_crash_installed = 1;
  g_compiler_argc = argc;
  g_compiler_argv = argv;

#if defined(_WIN32) || defined(_WIN64)
  SetUnhandledExceptionFilter(mettle_compiler_unhandled_exception_filter);
#else
  (void)mettle_install_signal_handler(
      SIGSEGV, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGBUS, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGFPE, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGILL, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGABRT, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
#endif
}
