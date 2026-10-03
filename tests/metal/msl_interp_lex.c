#include "msl_interp_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  MslProgram *prog;
  const char *src;
  size_t len;
  size_t pos;
  int line;
  int line_start;
  MslToken *toks;
  size_t count;
  size_t cap;
  char *error;
  size_t error_size;
} MslLexer;

static const char *const msl_puncts3[] = {"<<=", ">>="};
static const char *const msl_puncts2[] = {
  "::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^=", "++", "--", "->"
};
static const char msl_puncts1[] = "(){}[];,.:?+-*/%<>=!~&|^";

static const char *const msl_directives[] = {
  "include<metal_stdlib>",
  "pragmaMETALfpmath_mode(safe)",
  "pragmaMETALfpmath_mode(fast)",
  "pragmaMETALfpmath_mode(relaxed)",
  "pragmaMETALfpcontract(off)",
  "pragmaMETALfpcontract(on)",
  "pragmaMETALfpcontract(fast)"
};

static int lex_fail(MslLexer *lx, const char *fmt, ...) {
  char message[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  msl_format_error(lx->error, lx->error_size, "line %d: %s", lx->line, message);
  return 0;
}

static MslToken *push_token(MslLexer *lx, int kind, size_t start) {
  MslToken *tok;
  if (lx->count == lx->cap) {
    size_t cap = lx->cap == 0 ? 1024 : lx->cap * 2;
    MslToken *grown = (MslToken *)realloc(lx->toks, cap * sizeof(MslToken));
    if (grown == NULL) {
      return NULL;
    }
    lx->toks = grown;
    lx->cap = cap;
  }
  tok = &lx->toks[lx->count++];
  memset(tok, 0, sizeof *tok);
  tok->kind = kind;
  tok->line = lx->line;
  tok->text = lx->src + start;
  tok->len = lx->pos - start;
  return tok;
}

static int lex_directive(MslLexer *lx) {
  char compact[128];
  size_t used = 0;
  size_t i;
  lx->pos++;
  while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
    char ch = lx->src[lx->pos++];
    if (ch == '\r' || ch == ' ' || ch == '\t') {
      continue;
    }
    if (used + 1 >= sizeof compact) {
      return lex_fail(lx, "preprocessor directive outside the dialect");
    }
    compact[used++] = ch;
  }
  compact[used] = 0;
  for (i = 0; i < sizeof(msl_directives) / sizeof(msl_directives[0]); i++) {
    if (strcmp(compact, msl_directives[i]) == 0) {
      if (i == 0) {
        lx->prog->saw_include = 1;
      }
      return 1;
    }
  }
  return lex_fail(lx, "preprocessor directive '#%s' is outside the dialect", compact);
}

static int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static int lex_int_suffix(MslLexer *lx, MslToken *tok) {
  while (lx->pos < lx->len) {
    char ch = lx->src[lx->pos];
    if (ch == 'u' || ch == 'U') {
      if (tok->int_unsigned) {
        return lex_fail(lx, "invalid integer suffix");
      }
      tok->int_unsigned = 1;
    } else if (ch == 'l' || ch == 'L') {
      if (tok->int_long) {
        return lex_fail(lx, "integer suffix 'll' is outside the dialect");
      }
      tok->int_long = 1;
    } else {
      break;
    }
    lx->pos++;
  }
  return 1;
}

static int lex_float(MslLexer *lx, size_t start) {
  char buffer[128];
  size_t n = lx->pos - start;
  float value;
  MslToken *tok;
  if (lx->pos >= lx->len || (lx->src[lx->pos] != 'f' && lx->src[lx->pos] != 'F')) {
    return lex_fail(lx, "floating literal without 'f' suffix is a double, which is outside the dialect");
  }
  if (n >= sizeof buffer) {
    return lex_fail(lx, "floating literal too long");
  }
  memcpy(buffer, lx->src + start, n);
  buffer[n] = 0;
  value = strtof(buffer, NULL);
  if (value - value != 0.0f) {
    return lex_fail(lx, "floating literal %s is out of the range of float", buffer);
  }
  lx->pos++;
  tok = push_token(lx, MSL_TK_FLOAT, start);
  if (tok == NULL) {
    return lex_fail(lx, "out of memory");
  }
  tok->float_bits = msl_float_to_bits(value);
  return 1;
}

static int scan_digits(MslLexer *lx) {
  size_t start = lx->pos;
  while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) {
    lx->pos++;
  }
  return lx->pos > start;
}

static int lex_decimal(MslLexer *lx, size_t start, MslToken **out) {
  uint64_t value = 0;
  size_t i;
  if (lx->pos - start > 1 && lx->src[start] == '0') {
    return lex_fail(lx, "octal literals are outside the dialect");
  }
  for (i = start; i < lx->pos; i++) {
    uint64_t digit = (uint64_t)(lx->src[i] - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return lex_fail(lx, "integer literal too large");
    }
    value = value * 10u + digit;
  }
  *out = push_token(lx, MSL_TK_INT, start);
  if (*out == NULL) {
    return lex_fail(lx, "out of memory");
  }
  (*out)->int_value = value;
  return 1;
}

static int lex_hex(MslLexer *lx, size_t start, MslToken **out) {
  uint64_t value = 0;
  int digits = 0;
  lx->pos += 2;
  while (lx->pos < lx->len && hex_value(lx->src[lx->pos]) >= 0) {
    if (value >> 60) {
      return lex_fail(lx, "integer literal too large");
    }
    value = value * 16u + (uint64_t)hex_value(lx->src[lx->pos]);
    lx->pos++;
    digits++;
  }
  if (digits == 0) {
    return lex_fail(lx, "hex literal without digits");
  }
  *out = push_token(lx, MSL_TK_INT, start);
  if (*out == NULL) {
    return lex_fail(lx, "out of memory");
  }
  (*out)->int_value = value;
  (*out)->int_hex = 1;
  return 1;
}

static int lex_number(MslLexer *lx) {
  size_t start = lx->pos;
  MslToken *tok = NULL;
  int is_float = 0;
  if (lx->src[lx->pos] == '0' && lx->pos + 1 < lx->len && (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X')) {
    if (!lex_hex(lx, start, &tok)) {
      return 0;
    }
  } else {
    scan_digits(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
      lx->pos++;
      scan_digits(lx);
      is_float = 1;
    }
    if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
      lx->pos++;
      if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-')) {
        lx->pos++;
      }
      if (!scan_digits(lx)) {
        return lex_fail(lx, "malformed exponent");
      }
      is_float = 1;
    }
    if (is_float) {
      return lex_float(lx, start);
    }
    if (!lex_decimal(lx, start, &tok)) {
      return 0;
    }
  }
  if (!lex_int_suffix(lx, tok)) {
    return 0;
  }
  tok->len = lx->pos - start;
  if (lx->pos < lx->len && (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_')) {
    return lex_fail(lx, "invalid suffix on integer literal");
  }
  return 1;
}

static int lex_escape(MslLexer *lx, char *out) {
  char ch = lx->src[lx->pos++];
  switch (ch) {
    case 'n': *out = '\n'; return 1;
    case 't': *out = '\t'; return 1;
    case 'r': *out = '\r'; return 1;
    case '0': *out = '\0'; return 1;
    case '\\': *out = '\\'; return 1;
    case '"': *out = '"'; return 1;
    case '\'': *out = '\''; return 1;
    default: return lex_fail(lx, "escape sequence '\\%c' is outside the dialect", ch);
  }
}

static int lex_string(MslLexer *lx) {
  size_t start = lx->pos;
  char *buffer = (char *)malloc(lx->len - lx->pos + 1);
  size_t used = 0;
  MslToken *tok;
  if (buffer == NULL) {
    return lex_fail(lx, "out of memory");
  }
  lx->pos++;
  while (lx->pos < lx->len && lx->src[lx->pos] != '"') {
    char ch = lx->src[lx->pos];
    if (ch == '\n') {
      free(buffer);
      return lex_fail(lx, "unterminated string literal");
    }
    lx->pos++;
    if (ch == '\\') {
      if (lx->pos >= lx->len || !lex_escape(lx, &ch)) {
        free(buffer);
        return lx->pos >= lx->len ? lex_fail(lx, "unterminated string literal") : 0;
      }
    }
    buffer[used++] = ch;
  }
  if (lx->pos >= lx->len) {
    free(buffer);
    return lex_fail(lx, "unterminated string literal");
  }
  lx->pos++;
  tok = push_token(lx, MSL_TK_STRING, start);
  if (tok == NULL) {
    free(buffer);
    return lex_fail(lx, "out of memory");
  }
  tok->string_value = msl_arena_strndup(lx->prog, buffer, used);
  free(buffer);
  return tok->string_value != NULL ? 1 : lex_fail(lx, "out of memory");
}

static int lex_punct(MslLexer *lx) {
  size_t start = lx->pos;
  size_t i;
  size_t rest = lx->len - lx->pos;
  for (i = 0; i < sizeof(msl_puncts3) / sizeof(msl_puncts3[0]); i++) {
    if (rest >= 3 && memcmp(lx->src + lx->pos, msl_puncts3[i], 3) == 0) {
      lx->pos += 3;
      return push_token(lx, MSL_TK_PUNCT, start) != NULL ? 1 : lex_fail(lx, "out of memory");
    }
  }
  for (i = 0; i < sizeof(msl_puncts2) / sizeof(msl_puncts2[0]); i++) {
    if (rest >= 2 && memcmp(lx->src + lx->pos, msl_puncts2[i], 2) == 0) {
      lx->pos += 2;
      return push_token(lx, MSL_TK_PUNCT, start) != NULL ? 1 : lex_fail(lx, "out of memory");
    }
  }
  if (strchr(msl_puncts1, lx->src[lx->pos]) != NULL && lx->src[lx->pos] != 0) {
    lx->pos++;
    return push_token(lx, MSL_TK_PUNCT, start) != NULL ? 1 : lex_fail(lx, "out of memory");
  }
  if (lx->src[lx->pos] == '\'') {
    return lex_fail(lx, "character literals are outside the dialect");
  }
  return lex_fail(lx, "unexpected character '%c'", lx->src[lx->pos]);
}

static int skip_space(MslLexer *lx) {
  while (lx->pos < lx->len) {
    char ch = lx->src[lx->pos];
    if (ch == '\n') {
      lx->line++;
      lx->line_start = 1;
      lx->pos++;
    } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
      lx->pos++;
    } else if (ch == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
      while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
        lx->pos++;
      }
    } else if (ch == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
      return lex_fail(lx, "block comments are outside the dialect");
    } else if (ch == '#' && lx->line_start) {
      if (!lex_directive(lx)) {
        return 0;
      }
    } else {
      break;
    }
  }
  return 1;
}

static int lex_token(MslLexer *lx) {
  char ch = lx->src[lx->pos];
  size_t start = lx->pos;
  lx->line_start = 0;
  if (isalpha((unsigned char)ch) || ch == '_') {
    while (lx->pos < lx->len && (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_')) {
      lx->pos++;
    }
    return push_token(lx, MSL_TK_IDENT, start) != NULL ? 1 : lex_fail(lx, "out of memory");
  }
  if (isdigit((unsigned char)ch) || (ch == '.' && lx->pos + 1 < lx->len && isdigit((unsigned char)lx->src[lx->pos + 1]))) {
    return lex_number(lx);
  }
  if (ch == '"') {
    return lex_string(lx);
  }
  if (ch == '#') {
    return lex_fail(lx, "'#' must start a line");
  }
  return lex_punct(lx);
}

int msl_lex(MslProgram *prog, const char *source, size_t length, MslToken **tokens, size_t *count, char *error, size_t error_size) {
  MslLexer lx;
  MslToken *eof;
  memset(&lx, 0, sizeof lx);
  lx.prog = prog;
  lx.src = source;
  lx.len = length;
  lx.line = 1;
  lx.line_start = 1;
  lx.error = error;
  lx.error_size = error_size;
  for (;;) {
    if (!skip_space(&lx)) {
      break;
    }
    if (lx.pos >= lx.len) {
      eof = push_token(&lx, MSL_TK_EOF, lx.pos);
      if (eof == NULL) {
        lex_fail(&lx, "out of memory");
        break;
      }
      eof->text = "end of file";
      eof->len = strlen(eof->text);
      *tokens = lx.toks;
      *count = lx.count;
      return 1;
    }
    if (lx.src[lx.pos] == 0) {
      lex_fail(&lx, "NUL byte in source");
      break;
    }
    if (!lex_token(&lx)) {
      break;
    }
  }
  *tokens = lx.toks;
  *count = lx.count;
  return 0;
}
