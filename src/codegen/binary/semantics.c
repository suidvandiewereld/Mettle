
#include "internal.h"

#include <string.h>

uint64_t binary_semantics_float_sign_mask(int float_bits) {
  if (float_bits == 32) {
    return 0x80000000ull;
  }
  return 0x8000000000000000ull;
}

int binary_semantics_condition_code(const char *op, int is_unsigned,
                                    unsigned char *out) {
  unsigned char code;
  if (!op || !out) {
    return 0;
  }
  if (strcmp(op, "==") == 0) {
    code = 0x94;
  } else if (strcmp(op, "!=") == 0) {
    code = 0x95;
  } else if (strcmp(op, "<") == 0) {
    code = is_unsigned ? 0x92 : 0x9C;
  } else if (strcmp(op, "<=") == 0) {
    code = is_unsigned ? 0x96 : 0x9E;
  } else if (strcmp(op, ">") == 0) {
    code = is_unsigned ? 0x97 : 0x9F;
  } else if (strcmp(op, ">=") == 0) {
    code = is_unsigned ? 0x93 : 0x9D;
  } else {
    return 0;
  }
  *out = code;
  return 1;
}

int binary_semantics_is_comparison(const char *op) {
  unsigned char ignored;
  return binary_semantics_condition_code(op, 0, &ignored);
}
