#include "../src/compiler/compiler_context.h"
#include "../src/compiler/compiler_crash.h"

#include <stdio.h>
#include <string.h>

void mettle_crash_write_stderr_bytes(const char *text, size_t length);
void mettle_crash_write_stderr(const char *text);
long long (*mettle_crash_heap_classifier)(void *address) = NULL;

void mettle_crash_write_stderr_bytes(const char *text, size_t length) {
  fwrite(text, 1, length, stderr);
}

void mettle_crash_write_stderr(const char *text) {
  mettle_crash_write_stderr_bytes(text, strlen(text));
}

int mettle_path_exists(const char *path);
int mettle_path_is_directory(const char *path);
int mettle_make_directory(const char *path);
long long mettle_executable_path(char *buffer, unsigned long long size);
int mettle_getcwd(char *buffer, int size);
unsigned long long mettle_environment_write(char *buffer,
                                            unsigned long long size,
                                            const char *prefix);

int mettle_path_exists(const char *path) {
  (void)path;
  return 0;
}

int mettle_path_is_directory(const char *path) {
  (void)path;
  return 0;
}

int mettle_make_directory(const char *path) {
  (void)path;
  return -1;
}

long long mettle_executable_path(char *buffer, unsigned long long size) {
  (void)buffer;
  (void)size;
  return -1;
}

int mettle_getcwd(char *buffer, int size) {
  (void)buffer;
  (void)size;
  return -1;
}

unsigned long long mettle_environment_write(char *buffer,
                                            unsigned long long size,
                                            const char *prefix) {
  (void)prefix;
  if (buffer && size > 0) {
    buffer[0] = '\0';
  }
  return 0;
}

#if !defined(_WIN32) && !defined(_WIN64)

#include <signal.h>
#include <pthread.h>

int mettle_install_signal_handler(int signal_number,
                                  void (*handler)(int, void *, void *));
unsigned int mettle_thread_current_id(void);
int mettle_address_is_readable(const void *address, unsigned long long length);

int mettle_install_signal_handler(int signal_number,
                                  void (*handler)(int, void *, void *)) {
  (void)signal_number;
  (void)handler;
  return 1;
}

unsigned int mettle_thread_current_id(void) {
  return (unsigned int)(unsigned long)pthread_self();
}

int mettle_address_is_readable(const void *address, unsigned long long length) {
  return address != 0 && length > 0;
}
#endif

int main(void) {
  IRInstruction instruction = {0};
  char line[256];

  mettle_compiler_ctx_reset();
  mettle_compiler_ctx_set_input_filename("examples/grep/grep.mettle");
  mettle_compiler_ctx_set_current_filename("examples/grep/grep.mettle");
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_IR_OPTIMIZATION);
  mettle_compiler_ctx_set_pass_name("memcpy_inline");
  mettle_compiler_ctx_set_function_name("fill_buffer");
  mettle_compiler_ctx_set_options(1, 1);
  mettle_compiler_ctx_set_last_action(
      "collecting temp uses for IR_OP_MEMCPY_INLINE");

  instruction.op = IR_OP_MEMCPY_INLINE;
  instruction.dest = ir_operand_temp("tmp42");
  instruction.lhs = ir_operand_temp("src");
  instruction.rhs = ir_operand_temp("size");

  mettle_compiler_ctx_set_ir_instruction(184, &instruction);
  if (!ir_instruction_dump(&instruction, line, sizeof(line))) {
    fprintf(stderr, "ir_instruction_dump failed\n");
    return 1;
  }

  mettle_compiler_ice_report("access violation", "0xC0000005");
  return 0;
}
