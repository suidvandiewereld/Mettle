#include "tune.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define TUNE_MAX_ROWS 256
#define TUNE_MAX_COLUMNS 32
#define TUNE_MAX_FILES 64
#define TUNE_MAX_KEYS 64
#define TUNE_MAX_CASES 128
#define TUNE_MAX_ROUNDS 16
#define TUNE_MAX_SETS 64
#define TUNE_PATH 1024

typedef struct {
  char *name;
  char *value;
} TuneCell;

typedef struct {
  TuneCell cells[TUNE_MAX_COLUMNS];
  int count;
} TuneRow;

typedef struct {
  char path[TUNE_PATH];
  char type_name[256];
  TuneRow rows[TUNE_MAX_ROWS];
  int count;
} TuneTable;

typedef struct {
  const char *text;
  size_t at;
  size_t length;
  int kind;
  char token[2048];
} TuneLexer;

typedef struct {
  char name[96];
  double values[TUNE_MAX_ROUNDS];
  int count;
} TuneCase;

typedef struct {
  TuneCase cases[TUNE_MAX_CASES];
  int case_count;
  char verdict[400];
  int built;
} TuneRecord;

typedef struct {
  const char *module;
  const char *space;
  const char *key;
  const char *name_column;
  const char *build;
  const char *run;
  const char *artifact;
  const char *check;
  int rounds;
  double margin;
} TuneArgs;

typedef struct {
  TuneArgs args;
  const char *self_path;
  TuneTable *space;
  TuneRecord *records;
  char tune_path[TUNE_PATH + 32];
  char import_path[TUNE_PATH];
  char temp_directory[TUNE_PATH];
  char keys[TUNE_MAX_KEYS][256];
  int key_rows[TUNE_MAX_KEYS][TUNE_MAX_ROWS];
  int key_counts[TUNE_MAX_KEYS];
  int shipped[TUNE_MAX_KEYS];
  int chosen[TUNE_MAX_KEYS];
  int key_count;
  int sets[TUNE_MAX_SETS][TUNE_MAX_KEYS];
  int set_count;
  int built_sets[TUNE_MAX_SETS][TUNE_MAX_KEYS];
  char artifacts[TUNE_MAX_SETS][TUNE_PATH + 64];
  int built_count;
} TuneSession;

enum { TUNE_END, TUNE_IDENT, TUNE_NUMBER, TUNE_STRING, TUNE_PUNCT };

static char *tune_read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long size;
  char *text;
  if (!file) {
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  text = (char *)malloc((size_t)size + 1);
  if (!text) {
    fclose(file);
    return NULL;
  }
  if (fread(text, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    free(text);
    return NULL;
  }
  text[size] = '\0';
  fclose(file);
  return text;
}

static int tune_write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  size_t length = strlen(text);
  if (!file) {
    return 0;
  }
  if (fwrite(text, 1, length, file) != length) {
    fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static int tune_copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  FILE *out;
  char buffer[65536];
  size_t got;
  if (!in) {
    return 0;
  }
  out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return 0;
  }
  while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, got, out) != got) {
      fclose(in);
      fclose(out);
      return 0;
    }
  }
  fclose(in);
  if (fclose(out) != 0) {
    return 0;
  }
#ifndef _WIN32
  if (chmod(to, 0755) != 0) {
    return 0;
  }
#endif
  return 1;
}

static int tune_file_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  fclose(file);
  return 1;
}

static void tune_directory_of(const char *path, char *out, size_t size) {
  const char *slash = strrchr(path, '/');
  const char *back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) {
    slash = back;
  }
  if (!slash) {
    snprintf(out, size, ".");
    return;
  }
  snprintf(out, size, "%.*s", (int)(slash - path), path);
}

static size_t tune_skip_space(const char *text, size_t at, size_t length) {
  for (;;) {
    while (at < length && isspace((unsigned char)text[at])) {
      at++;
    }
    if (at + 1 < length && text[at] == '/' && text[at + 1] == '/') {
      while (at < length && text[at] != '\n') {
        at++;
      }
      continue;
    }
    if (at + 1 < length && text[at] == '/' && text[at + 1] == '*') {
      at += 2;
      while (at + 1 < length && !(text[at] == '*' && text[at + 1] == '/')) {
        at++;
      }
      at = at + 2 <= length ? at + 2 : length;
      continue;
    }
    break;
  }
  return at;
}

static void tune_next(TuneLexer *lexer) {
  const char *text = lexer->text;
  size_t at = tune_skip_space(text, lexer->at, lexer->length);
  size_t used = 0;
  lexer->token[0] = '\0';
  if (at >= lexer->length) {
    lexer->kind = TUNE_END;
    lexer->at = at;
    return;
  }
  if (isalpha((unsigned char)text[at]) || text[at] == '_') {
    lexer->kind = TUNE_IDENT;
    while (at < lexer->length &&
           (isalnum((unsigned char)text[at]) || text[at] == '_') &&
           used + 1 < sizeof(lexer->token)) {
      lexer->token[used++] = text[at++];
    }
  } else if (isdigit((unsigned char)text[at])) {
    lexer->kind = TUNE_NUMBER;
    while (at < lexer->length && used + 1 < sizeof(lexer->token) &&
           (isalnum((unsigned char)text[at]) || text[at] == '.' ||
            text[at] == '_' ||
            ((text[at] == '-' || text[at] == '+') && used > 0 &&
             (lexer->token[used - 1] == 'e' ||
              lexer->token[used - 1] == 'E')))) {
      lexer->token[used++] = text[at++];
    }
  } else if (text[at] == '"') {
    lexer->kind = TUNE_STRING;
    lexer->token[used++] = text[at++];
    while (at < lexer->length && text[at] != '"' &&
           used + 2 < sizeof(lexer->token)) {
      if (text[at] == '\\' && at + 1 < lexer->length) {
        lexer->token[used++] = text[at++];
      }
      lexer->token[used++] = text[at++];
    }
    if (at < lexer->length && text[at] == '"') {
      lexer->token[used++] = text[at++];
    }
  } else {
    lexer->kind = TUNE_PUNCT;
    lexer->token[used++] = text[at++];
  }
  lexer->token[used] = '\0';
  lexer->at = at;
}

static int tune_is(const TuneLexer *lexer, int kind, const char *token) {
  return lexer->kind == kind && strcmp(lexer->token, token) == 0;
}

static char *tune_strdup(const char *text) {
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);
  if (copy) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

static int tune_parse_value(TuneLexer *lexer, char *out, size_t size) {
  size_t used = 0;
  out[0] = '\0';
  if (tune_is(lexer, TUNE_PUNCT, "-")) {
    tune_next(lexer);
    if (lexer->kind != TUNE_NUMBER) {
      return 0;
    }
    snprintf(out, size, "-%s", lexer->token);
    tune_next(lexer);
    return 1;
  }
  if (lexer->kind == TUNE_NUMBER || lexer->kind == TUNE_STRING) {
    snprintf(out, size, "%s", lexer->token);
    tune_next(lexer);
    return 1;
  }
  if (lexer->kind != TUNE_IDENT) {
    return 0;
  }
  for (;;) {
    used += (size_t)snprintf(out + used, size - used, "%s", lexer->token);
    tune_next(lexer);
    if (!tune_is(lexer, TUNE_PUNCT, ".") || used + 2 >= size) {
      return 1;
    }
    used += (size_t)snprintf(out + used, size - used, ".");
    tune_next(lexer);
    if (lexer->kind != TUNE_IDENT) {
      return 0;
    }
  }
}

static int tune_parse_rows(TuneLexer *lexer, TuneTable *table) {
  table->count = 0;
  while (!tune_is(lexer, TUNE_PUNCT, "]")) {
    TuneRow *row;
    if (table->count >= TUNE_MAX_ROWS || !tune_is(lexer, TUNE_PUNCT, "{")) {
      return 0;
    }
    row = &table->rows[table->count++];
    row->count = 0;
    tune_next(lexer);
    while (!tune_is(lexer, TUNE_PUNCT, "}")) {
      char value[2048];
      char name[256];
      if (lexer->kind != TUNE_IDENT || row->count >= TUNE_MAX_COLUMNS) {
        return 0;
      }
      snprintf(name, sizeof(name), "%s", lexer->token);
      tune_next(lexer);
      if (!tune_is(lexer, TUNE_PUNCT, ":")) {
        return 0;
      }
      tune_next(lexer);
      if (!tune_parse_value(lexer, value, sizeof(value))) {
        return 0;
      }
      row->cells[row->count].name = tune_strdup(name);
      row->cells[row->count].value = tune_strdup(value);
      if (!row->cells[row->count].name || !row->cells[row->count].value) {
        return 0;
      }
      row->count++;
      if (tune_is(lexer, TUNE_PUNCT, ",")) {
        tune_next(lexer);
      }
    }
    tune_next(lexer);
    if (tune_is(lexer, TUNE_PUNCT, ",")) {
      tune_next(lexer);
    }
  }
  return 1;
}

static int tune_find_table(const char *path, const char *text, const char *name,
                           TuneTable *table) {
  TuneLexer lexer;
  memset(&lexer, 0, sizeof(lexer));
  lexer.text = text;
  lexer.length = strlen(text);
  tune_next(&lexer);
  while (lexer.kind != TUNE_END) {
    if (!tune_is(&lexer, TUNE_IDENT, "const")) {
      tune_next(&lexer);
      continue;
    }
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_IDENT, name)) {
      continue;
    }
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_PUNCT, ":")) {
      continue;
    }
    tune_next(&lexer);
    if (lexer.kind != TUNE_IDENT) {
      continue;
    }
    snprintf(table->type_name, sizeof(table->type_name), "%s", lexer.token);
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_PUNCT, "[")) {
      continue;
    }
    tune_next(&lexer);
    if (lexer.kind != TUNE_NUMBER) {
      continue;
    }
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_PUNCT, "]")) {
      continue;
    }
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_PUNCT, "=")) {
      continue;
    }
    tune_next(&lexer);
    if (!tune_is(&lexer, TUNE_PUNCT, "[")) {
      continue;
    }
    tune_next(&lexer);
    snprintf(table->path, sizeof(table->path), "%s", path);
    return tune_parse_rows(&lexer, table) ? 1 : -1;
  }
  return 0;
}

static int tune_collect_imports(const char *path, const char *text,
                                char files[][TUNE_PATH], int *count) {
  TuneLexer lexer;
  char directory[TUNE_PATH];
  tune_directory_of(path, directory, sizeof(directory));
  memset(&lexer, 0, sizeof(lexer));
  lexer.text = text;
  lexer.length = strlen(text);
  tune_next(&lexer);
  while (lexer.kind != TUNE_END) {
    if (tune_is(&lexer, TUNE_IDENT, "import")) {
      tune_next(&lexer);
      if (lexer.kind == TUNE_STRING) {
        char name[TUNE_PATH];
        char candidate[TUNE_PATH * 2];
        size_t length;
        snprintf(name, sizeof(name), "%.*s", (int)strlen(lexer.token) - 2,
                 lexer.token + 1);
        length = strlen(name);
        if (strncmp(name, "std/", 4) != 0) {
          snprintf(candidate, sizeof(candidate), "%s/%s%s", directory, name,
                   length > 7 && strcmp(name + length - 7, ".mettle") == 0
                       ? ""
                       : ".mettle");
          if (tune_file_exists(candidate) && *count < TUNE_MAX_FILES &&
              strlen(candidate) < TUNE_PATH) {
            int known = 0;
            for (int i = 0; i < *count; i++) {
              if (strcmp(files[i], candidate) == 0) {
                known = 1;
              }
            }
            if (!known) {
              snprintf(files[(*count)++], TUNE_PATH, "%s", candidate);
            }
          }
        }
      }
    }
    tune_next(&lexer);
  }
  return 1;
}

static const char *tune_cell(const TuneRow *row, const char *name) {
  for (int i = 0; i < row->count; i++) {
    if (strcmp(row->cells[i].name, name) == 0) {
      return row->cells[i].value;
    }
  }
  return NULL;
}

static int tune_rows_equal(const TuneRow *a, const TuneRow *b) {
  if (a->count != b->count) {
    return 0;
  }
  for (int i = 0; i < a->count; i++) {
    const char *other = tune_cell(b, a->cells[i].name);
    if (!other || strcmp(other, a->cells[i].value) != 0) {
      return 0;
    }
  }
  return 1;
}

static void tune_row_text(const TuneRow *row, char *out, size_t size) {
  size_t used = (size_t)snprintf(out, size, "{ ");
  for (int i = 0; i < row->count && used < size; i++) {
    used += (size_t)snprintf(out + used, size - used, "%s%s: %s",
                             i ? ", " : "", row->cells[i].name,
                             row->cells[i].value);
  }
  if (used < size) {
    snprintf(out + used, size - used, " }");
  }
}

static void tune_row_name(const TuneRow *row, const char *column, char *out,
                          size_t size) {
  const char *value = tune_cell(row, column);
  size_t length = value ? strlen(value) : 0;
  if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
    snprintf(out, size, "%.*s", (int)length - 2, value + 1);
  } else {
    snprintf(out, size, "%s", value ? value : "");
  }
}

static int tune_identifier_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

static int tune_kernel_matches(const char *kernel, size_t length,
                               const char *name) {
  size_t name_length = strlen(name);
  if (name_length == 0 || length < name_length) {
    return 0;
  }
  if (strncmp(kernel + length - name_length, name, name_length) != 0) {
    return 0;
  }
  return length == name_length || kernel[length - name_length - 1] == '_';
}

static int tune_text_names(const char *text, const char *name) {
  size_t name_length = strlen(name);
  const char *at = text;
  while (name_length && (at = strstr(at, name)) != NULL) {
    char after = at[name_length];
    if (at > text && at[-1] == '_' && !tune_identifier_char(after)) {
      return 1;
    }
    at += name_length;
  }
  return 0;
}

static char *tune_run(const char *command, int *status) {
  char *full;
  char *output = NULL;
  size_t used = 0, capacity = 0;
  char buffer[4096];
  FILE *pipe;
  size_t length = strlen(command) + 16;
  full = (char *)malloc(length);
  if (!full) {
    return NULL;
  }
#ifdef _WIN32
  snprintf(full, length, "\"%s 2>&1\"", command);
  pipe = _popen(full, "r");
#else
  snprintf(full, length, "%s 2>&1", command);
  pipe = popen(full, "r");
#endif
  free(full);
  if (!pipe) {
    *status = -1;
    return NULL;
  }
  while (fgets(buffer, sizeof(buffer), pipe)) {
    size_t got = strlen(buffer);
    if (used + got + 1 > capacity) {
      size_t next = capacity ? capacity * 2 : 65536;
      char *grown;
      while (next < used + got + 1) {
        next *= 2;
      }
      grown = (char *)realloc(output, next);
      if (!grown) {
        break;
      }
      output = grown;
      capacity = next;
    }
    memcpy(output + used, buffer, got);
    used += got;
    output[used] = '\0';
  }
#ifdef _WIN32
  *status = _pclose(pipe);
#else
  *status = pclose(pipe);
#endif
  if (!output) {
    output = tune_strdup("");
  }
  return output;
}

static void tune_escape(const char *text, char *out, size_t size) {
  size_t used = 0;
  for (; *text && used + 3 < size; text++) {
    char c = *text;
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
    if (c == '"' || c == '\\') {
      out[used++] = '\\';
    }
    out[used++] = c;
  }
  out[used] = '\0';
}

static void tune_first_error(const char *output, char *out, size_t size) {
  const char *at = strstr(output, "error[");
  const char *end;
  if (!at) {
    snprintf(out, size, "the check failed");
    return;
  }
  end = strstr(at, "\n  --> ");
  if (!end) {
    end = strchr(at, '\n');
  }
  {
    const char *claim = strstr(at, ": output [");
    const char *apart = strstr(at, " differently.");
    if (apart && (!end || apart < end)) {
      end = apart + strlen(" differently");
    } else if (claim && (!end || claim < end)) {
      end = claim;
    }
  }
  snprintf(out, size, "%.*s", end ? (int)(end - at) : (int)strlen(at), at);
}

static int tune_proven(const char *output, const char *name) {
  const char *at = output;
  while ((at = strstr(at, ": proven for ")) != NULL) {
    const char *line_end;
    const char *start = at + strlen(": proven for ");
    line_end = strchr(start, '\n');
    if (!line_end) {
      line_end = start + strlen(start);
    }
    while (start < line_end) {
      const char *comma = start;
      while (comma < line_end && *comma != ',' && *comma != '\r') {
        comma++;
      }
      if (tune_kernel_matches(start, (size_t)(comma - start), name)) {
        return 1;
      }
      start = comma;
      while (start < line_end && (*start == ',' || *start == ' ' ||
                                  *start == '\r')) {
        start++;
      }
    }
    at = line_end;
  }
  return 0;
}

static const char *tune_word(const char *at, char *out, size_t size) {
  size_t used = 0;
  while (*at == ' ' || *at == '\t') {
    at++;
  }
  while (*at && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n') {
    if (used + 1 < size) {
      out[used++] = *at;
    }
    at++;
  }
  out[used] = '\0';
  return at;
}

static int tune_parse_time(const char *line, char *kernel, size_t kernel_size,
                           char *label, size_t label_size, double *value) {
  char word[64];
  char *end = NULL;
  const char *at = tune_word(line, word, sizeof(word));
  if (strcmp(word, "tune") != 0) {
    return 0;
  }
  at = tune_word(at, kernel, kernel_size);
  at = tune_word(at, label, label_size);
  at = tune_word(at, word, sizeof(word));
  if (!kernel[0] || !label[0] || !word[0]) {
    return 0;
  }
  *value = strtod(word, &end);
  return end && end != word && *end == '\0';
}

static int tune_compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static double tune_median(const TuneCase *entry) {
  double sorted[TUNE_MAX_ROUNDS];
  memcpy(sorted, entry->values, (size_t)entry->count * sizeof(double));
  qsort(sorted, (size_t)entry->count, sizeof(double), tune_compare_double);
  if (entry->count % 2) {
    return sorted[entry->count / 2];
  }
  return (sorted[entry->count / 2 - 1] + sorted[entry->count / 2]) / 2.0;
}

static double tune_total(const TuneRecord *record) {
  double total = 0.0;
  for (int i = 0; i < record->case_count; i++) {
    total += tune_median(&record->cases[i]);
  }
  return total;
}

static void tune_add_time(TuneRecord *record, const char *name, double value) {
  int i;
  for (i = 0; i < record->case_count; i++) {
    if (strcmp(record->cases[i].name, name) == 0) {
      break;
    }
  }
  if (i == record->case_count) {
    if (record->case_count >= TUNE_MAX_CASES) {
      return;
    }
    snprintf(record->cases[i].name, sizeof(record->cases[i].name), "%s", name);
    record->cases[i].count = 0;
    record->case_count++;
  }
  if (record->cases[i].count < TUNE_MAX_ROUNDS) {
    record->cases[i].values[record->cases[i].count++] = value;
  }
}

static char *tune_render(const TuneArgs *args, const TuneTable *space,
                         const char *import_path, const int *chosen,
                         int chosen_count, const TuneRecord *records,
                         int with_results) {
  size_t capacity = 65536 + (size_t)space->count * 1024;
  char *out = (char *)malloc(capacity);
  size_t used = 0;
  char row[4096];
  if (!out) {
    return NULL;
  }
  used += (size_t)snprintf(out + used, capacity - used, "import \"%s\";\n",
                           import_path);
  if (with_results) {
    used += (size_t)snprintf(out + used, capacity - used,
                             "import \"std/tune\";\n");
  }
  used += (size_t)snprintf(out + used, capacity - used,
                           "\nexport const %s_TUNED: %s[%d] = [\n",
                           args->space, space->type_name, chosen_count);
  for (int i = 0; i < chosen_count; i++) {
    tune_row_text(&space->rows[chosen[i]], row, sizeof(row));
    used += (size_t)snprintf(out + used, capacity - used, "  %s%s\n", row,
                             i + 1 < chosen_count ? "," : "");
  }
  used += (size_t)snprintf(out + used, capacity - used, "];\n");
  if (with_results) {
    used += (size_t)snprintf(out + used, capacity - used,
                             "\nexport const %s_MEASURED: TuneResult[%d] = [\n",
                             args->space, space->count);
    for (int r = 0; r < space->count; r++) {
      char verdict[512];
      int is_chosen = 0;
      const char *key = tune_cell(&space->rows[r], args->key);
      char key_text[256];
      for (int i = 0; i < chosen_count; i++) {
        if (chosen[i] == r) {
          is_chosen = 1;
        }
      }
      tune_escape(records[r].verdict[0] ? records[r].verdict : "not measured",
                  verdict, sizeof(verdict));
      tune_escape(key ? key : "", key_text, sizeof(key_text));
      used += (size_t)snprintf(
          out + used, capacity - used,
          "  { row: %d, key: \"%s\", verdict: \"%s\", median_us: %.2f, "
          "chosen: %s }%s\n",
          r, key_text, verdict,
          records[r].case_count ? tune_total(&records[r]) : 0.0,
          is_chosen ? "true" : "false", r + 1 < space->count ? "," : "");
    }
    used += (size_t)snprintf(out + used, capacity - used, "];\n");
  }
  return out;
}

static void tune_usage(void) {
  fprintf(stderr,
          "usage: mettle tune <module.mettle> --space <TABLE> --key <column> "
          "--build <command> --artifact <path> --run <command> "
          "[--rounds N] [--check <compile flags>] [--name <column>] "
          "[--margin <percent>]\n"
          "  Tries the rows of TABLE, one row per key value in each build, "
          "and writes <module>.tune.mettle:\n"
          "  TABLE_TUNED holds the chosen row for every key, TABLE_MEASURED "
          "every row's verdict and time.\n"
          "  A row whose kernels a numerics contract does not prove is "
          "refused and never timed. --run is\n"
          "  run with {artifact} replaced by a copy of each build's "
          "artifact, and prints lines\n"
          "  'tune <kernel> <case> <microseconds>'.\n");
}

static int tune_parse_args(int argc, char **argv, TuneArgs *args) {
  memset(args, 0, sizeof(*args));
  args->rounds = 3;
  args->margin = 1.0;
  args->name_column = "name";
  args->check = "";
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (a[0] != '-' && !args->module) {
      args->module = a;
      continue;
    }
    if (!value) {
      return 0;
    }
    if (!strcmp(a, "--space")) {
      args->space = value;
    } else if (!strcmp(a, "--key")) {
      args->key = value;
    } else if (!strcmp(a, "--build")) {
      args->build = value;
    } else if (!strcmp(a, "--run")) {
      args->run = value;
    } else if (!strcmp(a, "--artifact")) {
      args->artifact = value;
    } else if (!strcmp(a, "--check")) {
      args->check = value;
    } else if (!strcmp(a, "--name")) {
      args->name_column = value;
    } else if (!strcmp(a, "--rounds")) {
      args->rounds = atoi(value);
    } else if (!strcmp(a, "--margin")) {
      args->margin = strtod(value, NULL);
    } else {
      return 0;
    }
    i++;
  }
  if (args->rounds < 1) {
    args->rounds = 1;
  }
  if (args->rounds > TUNE_MAX_ROUNDS) {
    args->rounds = TUNE_MAX_ROUNDS;
  }
  return args->module && args->space && args->key && args->build &&
         args->run && args->artifact;
}

static void tune_restore(const char *path, const char *original) {
  if (original) {
    tune_write_text(path, original);
  } else {
    remove(path);
  }
}

static char *tune_replace(const char *text, const char *needle,
                          const char *with) {
  size_t count = 0, needle_length = strlen(needle), with_length = strlen(with);
  const char *at = text;
  char *out, *write;
  while ((at = strstr(at, needle)) != NULL) {
    count++;
    at += needle_length;
  }
  out = (char *)malloc(strlen(text) + count * (with_length + 2) + 1);
  if (!out) {
    return NULL;
  }
  write = out;
  at = text;
  for (;;) {
    const char *next = strstr(at, needle);
    if (!next) {
      strcpy(write, at);
      break;
    }
    memcpy(write, at, (size_t)(next - at));
    write += next - at;
    *write++ = '"';
    memcpy(write, with, with_length);
    write += with_length;
    *write++ = '"';
    at = next + needle_length;
  }
  return out;
}

static int tune_locate_space(const TuneArgs *args, TuneTable *space) {
  static char files[TUNE_MAX_FILES][TUNE_PATH];
  int file_count = 1;
  int found = 0;
  snprintf(files[0], TUNE_PATH, "%s", args->module);
  for (int i = 0; i < file_count && !found; i++) {
    char *text = tune_read_file(files[i]);
    int result;
    if (!text) {
      fprintf(stderr, "error: cannot read '%s'\n", files[i]);
      return 0;
    }
    result = tune_find_table(files[i], text, args->space, space);
    if (result < 0) {
      fprintf(stderr,
              "error: '%s' in '%s' is not a table of rows this tuner can "
              "read: write it as a const array literal of struct rows\n",
              args->space, files[i]);
      free(text);
      return 0;
    }
    found = result > 0;
    if (!found) {
      tune_collect_imports(files[i], text, files, &file_count);
    }
    free(text);
  }
  if (!found || space->count == 0) {
    fprintf(stderr,
            "error: no table named '%s' in '%s' or the modules it imports\n",
            args->space, args->module);
    return 0;
  }
  return 1;
}

static int tune_resolve_paths(TuneSession *session) {
  const TuneArgs *args = &session->args;
  const TuneTable *space = session->space;
  char module_directory[TUNE_PATH];
  char space_directory[TUNE_PATH];
  tune_directory_of(args->module, module_directory, sizeof(module_directory));
  tune_directory_of(space->path, space_directory, sizeof(space_directory));
  {
    size_t length = strlen(args->module);
    if (length > 7 && strcmp(args->module + length - 7, ".mettle") == 0) {
      length -= 7;
    }
    snprintf(session->tune_path, sizeof(session->tune_path), "%.*s.tune.mettle",
             (int)length, args->module);
  }
  if (strcmp(module_directory, space_directory) != 0) {
    fprintf(stderr,
            "error: '%s' is declared in '%s'; the tuner writes its import "
            "into '%s', so declare the table beside the module\n",
            args->space, space->path, session->tune_path);
    return 0;
  }
  {
    const char *base = space->path + strlen(space_directory);
    size_t length;
    if (*base == '/' || *base == '\\') {
      base++;
    }
    length = strlen(base);
    if (length > 7 && strcmp(base + length - 7, ".mettle") == 0) {
      length -= 7;
    }
    snprintf(session->import_path, sizeof(session->import_path), "%.*s",
             (int)length, base);
  }
  return 1;
}

static int tune_group_keys(TuneSession *session) {
  const TuneArgs *args = &session->args;
  const TuneTable *space = session->space;
  for (int r = 0; r < space->count; r++) {
    const char *key = tune_cell(&space->rows[r], args->key);
    char name[256];
    int k;
    if (!key) {
      fprintf(stderr, "error: row %d of '%s' has no column '%s'\n", r,
              args->space, args->key);
      return 0;
    }
    tune_row_name(&space->rows[r], args->name_column, name, sizeof(name));
    if (!name[0]) {
      fprintf(stderr,
              "error: row %d of '%s' has no column '%s'; the tuner names a "
              "row's kernels by it\n",
              r, args->space, args->name_column);
      return 0;
    }
    for (k = 0; k < session->key_count; k++) {
      if (strcmp(session->keys[k], key) == 0) {
        break;
      }
    }
    if (k == session->key_count) {
      if (session->key_count >= TUNE_MAX_KEYS) {
        fprintf(stderr, "error: '%s' has more than %d keys\n", args->space,
                TUNE_MAX_KEYS);
        return 0;
      }
      snprintf(session->keys[k], sizeof(session->keys[k]), "%s", key);
      session->key_counts[k] = 0;
      session->shipped[k] = r;
      session->key_count++;
    }
    session->key_rows[k][session->key_counts[k]++] = r;
  }
  return 1;
}

static void tune_find_shipped(TuneSession *session, const char *original) {
  static TuneTable current;
  const TuneTable *space = session->space;
  char tuned_name[300];
  int result;
  snprintf(tuned_name, sizeof(tuned_name), "%s_TUNED", session->args.space);
  result = tune_find_table(session->tune_path, original, tuned_name, &current);
  if (result <= 0) {
    return;
  }
  for (int c = 0; c < current.count; c++) {
    for (int r = 0; r < space->count; r++) {
      const char *key = tune_cell(&space->rows[r], session->args.key);
      if (tune_rows_equal(&current.rows[c], &space->rows[r])) {
        for (int k = 0; k < session->key_count; k++) {
          if (strcmp(session->keys[k], key) == 0) {
            session->shipped[k] = r;
          }
        }
      }
    }
  }
}

static void tune_temp_directory(char *out, size_t size) {
  const char *temp_env = getenv("TEMP");
  if (!temp_env) {
    temp_env = getenv("TMPDIR");
  }
  snprintf(out, size, "%s", temp_env ? temp_env : ".");
}

static void tune_plan_sets(TuneSession *session) {
  int most = 0;
  for (int k = 0; k < session->key_count; k++) {
    if (session->key_counts[k] > most) {
      most = session->key_counts[k];
    }
  }
  for (int i = 0; i < most && i < TUNE_MAX_SETS; i++) {
    for (int k = 0; k < session->key_count; k++) {
      session->sets[i][k] = i < session->key_counts[k]
                                ? session->key_rows[k][i]
                                : session->shipped[k];
    }
  }
  session->set_count = most < TUNE_MAX_SETS ? most : TUNE_MAX_SETS;
}

static int tune_refuse_check_errors(TuneSession *session, int s,
                                    const char *output) {
  const TuneArgs *args = &session->args;
  TuneRecord *records = session->records;
  int *set = session->sets[s];
  char message[400];
  int refused = 0;
  tune_first_error(output, message, sizeof(message));
  for (int k = 0; k < session->key_count; k++) {
    char name[256];
    int r = set[k];
    tune_row_name(&session->space->rows[r], args->name_column, name,
                  sizeof(name));
    if (strstr(message, "error[C0") && tune_text_names(message, name)) {
      if (r == session->shipped[k]) {
        fprintf(stderr,
                "error: the shipped row %d for %s=%s is refused: %s\n", r,
                args->key, session->keys[k], message);
        return -1;
      }
      snprintf(records[r].verdict, sizeof(records[r].verdict), "refused: %s",
               message);
      printf("tune: row %d refused, never timed: %s\n", r, message);
      set[k] = session->shipped[k];
      refused = 1;
    }
  }
  if (!refused) {
    fprintf(stderr, "error: the module does not build:\n%s\n", output);
    return -1;
  }
  return 1;
}

static int tune_refuse_unproven(TuneSession *session, int s,
                                const char *output) {
  const TuneArgs *args = &session->args;
  TuneRecord *records = session->records;
  int *set = session->sets[s];
  int refused = 0;
  for (int k = 0; k < session->key_count; k++) {
    char name[256];
    int r = set[k];
    tune_row_name(&session->space->rows[r], args->name_column, name,
                  sizeof(name));
    if (!tune_proven(output, name)) {
      if (r == session->shipped[k]) {
        fprintf(stderr,
                "error: no numerics contract proves the kernels of %s's "
                "row %d, so tuning would choose between different "
                "results; put them under a contract\n",
                args->space, r);
        return -1;
      }
      snprintf(records[r].verdict, sizeof(records[r].verdict),
               "refused: no numerics contract proves its kernels");
      printf("tune: row %d refused, never timed: no numerics contract "
             "proves its kernels\n",
             r);
      set[k] = session->shipped[k];
      refused = 1;
    }
  }
  return refused;
}

static int tune_prove_attempt(TuneSession *session, int s) {
  const TuneArgs *args = &session->args;
  char *text = tune_render(args, session->space, session->import_path,
                           session->sets[s], session->key_count,
                           session->records, 0);
  char command[8192];
  char check_ptx[TUNE_PATH + 64];
  char *output;
  int status = 0;
  int verdict;
  if (!text || !tune_write_text(session->tune_path, text)) {
    fprintf(stderr, "error: cannot write '%s'\n", session->tune_path);
    free(text);
    return -1;
  }
  free(text);
  snprintf(check_ptx, sizeof(check_ptx), "%s/mettle_tune_check.ptx",
           session->temp_directory);
  snprintf(command, sizeof(command),
           "\"%s\" --emit-ptx --report-gpu-types %s \"%s\" -o \"%s\"",
           session->self_path, args->check, args->module, check_ptx);
  printf("tune: proving build %d:", s + 1);
  for (int k = 0; k < session->key_count; k++) {
    printf(" %s=row %d", session->keys[k], session->sets[s][k]);
  }
  printf("\n");
  fflush(stdout);
  output = tune_run(command, &status);
  if (!output) {
    fprintf(stderr, "error: cannot run the check: %s\n", command);
    return -1;
  }
  if (status != 0) {
    verdict = tune_refuse_check_errors(session, s, output);
  } else {
    verdict = tune_refuse_unproven(session, s, output);
  }
  free(output);
  return verdict;
}

static int tune_prove_set(TuneSession *session, int s) {
  int verdict;
  do {
    verdict = tune_prove_attempt(session, s);
  } while (verdict > 0);
  return verdict == 0;
}

static int tune_build_set(TuneSession *session, int s) {
  const TuneArgs *args = &session->args;
  TuneRecord *records = session->records;
  int *set = session->sets[s];
  int built_count = session->built_count;
  int status = 0;
  char *output;
  printf("tune: building %d\n", built_count + 1);
  fflush(stdout);
  output = tune_run(args->build, &status);
  if (!output || status != 0) {
    fprintf(stderr, "error: the build step failed:\n%s\n",
            output ? output : "");
    free(output);
    return 0;
  }
  free(output);
  {
    char artifact_directory[TUNE_PATH];
    tune_directory_of(args->artifact, artifact_directory,
                      sizeof(artifact_directory));
    snprintf(session->artifacts[built_count],
             sizeof(session->artifacts[built_count]), "%s/mettle_tune_%d_%s",
             artifact_directory, built_count + 1,
             strrchr(args->artifact, '/') ? strrchr(args->artifact, '/') + 1
             : strrchr(args->artifact, '\\')
                 ? strrchr(args->artifact, '\\') + 1
                 : args->artifact);
  }
  if (!tune_copy_file(args->artifact, session->artifacts[built_count])) {
    fprintf(stderr, "error: the build step left no '%s'\n", args->artifact);
    return 0;
  }
  memcpy(session->built_sets[built_count], set,
         sizeof(int) * (size_t)session->key_count);
  for (int k = 0; k < session->key_count; k++) {
    records[set[k]].built = 1;
    if (!records[set[k]].verdict[0]) {
      snprintf(records[set[k]].verdict, sizeof(records[set[k]].verdict),
               "proven");
    }
  }
  session->built_count++;
  return 1;
}

static int tune_build_sets(TuneSession *session) {
  for (int s = 0; s < session->set_count; s++) {
    int duplicate = 0;
    if (!tune_prove_set(session, s)) {
      return 0;
    }
    for (int b = 0; b < session->built_count; b++) {
      if (memcmp(session->built_sets[b], session->sets[s],
                 sizeof(int) * (size_t)session->key_count) == 0) {
        duplicate = 1;
      }
    }
    if (duplicate) {
      continue;
    }
    if (!tune_build_set(session, s)) {
      return 0;
    }
  }
  return 1;
}

static int tune_record_times(TuneSession *session, int b, const char *output) {
  const TuneArgs *args = &session->args;
  int lines = 0;
  const char *at = output;
  while (at && *at) {
    const char *end = strchr(at, '\n');
    char line[512];
    char kernel[256], label[96];
    double value = 0.0;
    if (!end) {
      end = at + strlen(at);
    }
    snprintf(line, sizeof(line), "%.*s", (int)(end - at), at);
    if (tune_parse_time(line, kernel, sizeof(kernel), label, sizeof(label),
                        &value)) {
      for (int k = 0; k < session->key_count; k++) {
        char name[256];
        int r = session->built_sets[b][k];
        tune_row_name(&session->space->rows[r], args->name_column, name,
                      sizeof(name));
        if (tune_kernel_matches(kernel, strlen(kernel), name)) {
          tune_add_time(&session->records[r], label, value);
          lines++;
        }
      }
    }
    at = *end ? end + 1 : end;
  }
  return lines;
}

static int tune_time_build(TuneSession *session, int b) {
  char *command =
      tune_replace(session->args.run, "{artifact}", session->artifacts[b]);
  char *output;
  int status = 0;
  if (!command) {
    return 0;
  }
  output = tune_run(command, &status);
  free(command);
  if (!output || status != 0) {
    fprintf(stderr,
            "error: the run step failed; tuning runs on the device:\n%s\n",
            output ? output : "");
    free(output);
    return 0;
  }
  if (tune_record_times(session, b, output) == 0) {
    fprintf(stderr,
            "error: the run step printed no 'tune <kernel> <case> "
            "<microseconds>' lines for these rows; it printed:\n%.4000s\n",
            output);
    free(output);
    return 0;
  }
  free(output);
  return 1;
}

static int tune_time_round(TuneSession *session, int round) {
  printf("tune: timing round %d of %d\n", round + 1, session->args.rounds);
  fflush(stdout);
  for (int b = 0; b < session->built_count; b++) {
    if (!tune_time_build(session, b)) {
      return 0;
    }
  }
  return 1;
}

static void tune_choose_rows(TuneSession *session) {
  const TuneRecord *records = session->records;
  for (int k = 0; k < session->key_count; k++) {
    int best = session->shipped[k];
    double shipped_total = tune_total(&records[session->shipped[k]]);
    double best_total = shipped_total;
    for (int i = 0; i < session->key_counts[k]; i++) {
      int r = session->key_rows[k][i];
      double total;
      if (!records[r].built || records[r].case_count == 0 ||
          strcmp(records[r].verdict, "proven") != 0) {
        continue;
      }
      total = tune_total(&records[r]);
      if (total < best_total) {
        best_total = total;
        best = r;
      }
    }
    if (best != session->shipped[k] &&
        best_total > shipped_total * (1.0 - session->args.margin / 100.0)) {
      best = session->shipped[k];
    }
    session->chosen[k] = best;
  }
}

static void tune_print_summary(const TuneSession *session) {
  const TuneRecord *records = session->records;
  for (int k = 0; k < session->key_count; k++) {
    double shipped_total = tune_total(&records[session->shipped[k]]);
    printf("%s = %s\n", session->args.key, session->keys[k]);
    for (int i = 0; i < session->key_counts[k]; i++) {
      int r = session->key_rows[k][i];
      char row[2048];
      tune_row_text(&session->space->rows[r], row, sizeof(row));
      if (records[r].case_count) {
        double total = tune_total(&records[r]);
        printf("  row %2d %9.2f us  %+6.1f%%  %s%s%s\n", r, total,
               shipped_total > 0.0 ? (total / shipped_total - 1.0) * 100.0
                                   : 0.0,
               row, r == session->shipped[k] ? "  (shipped)" : "",
               r == session->chosen[k] ? "  <- chosen" : "");
      } else {
        printf("  row %2d   %s  %s\n", r,
               records[r].verdict[0] ? records[r].verdict : "not measured",
               row);
      }
    }
  }
}

int mettle_tune_main(int argc, char **argv, const char *self_path) {
  static TuneTable space;
  static TuneRecord records[TUNE_MAX_ROWS];
  TuneSession session;
  int failed = 0;
  char *original;

  if (!tune_parse_args(argc, argv, &session.args)) {
    tune_usage();
    return 1;
  }
  memset(records, 0, sizeof(records));
  session.self_path = self_path;
  session.space = &space;
  session.records = records;
  session.key_count = 0;
  session.set_count = 0;
  session.built_count = 0;
  if (!tune_locate_space(&session.args, &space)) {
    return 1;
  }
  if (!tune_resolve_paths(&session)) {
    return 1;
  }
  if (!tune_group_keys(&session)) {
    return 1;
  }

  original = tune_read_file(session.tune_path);
  if (original) {
    tune_find_shipped(&session, original);
  }

  tune_temp_directory(session.temp_directory, sizeof(session.temp_directory));
  tune_plan_sets(&session);

  printf("tune: %s, %d rows over %d values of '%s'; writing %s\n",
         session.args.space, space.count, session.key_count, session.args.key,
         session.tune_path);
  fflush(stdout);

  failed = !tune_build_sets(&session);
  for (int round = 0; round < session.args.rounds && !failed; round++) {
    failed = !tune_time_round(&session, round);
  }

  if (failed) {
    tune_restore(session.tune_path, original);
    for (int b = 0; b < session.built_count; b++) {
      remove(session.artifacts[b]);
    }
    free(original);
    return 1;
  }

  tune_choose_rows(&session);

  {
    char *text = tune_render(&session.args, &space, session.import_path,
                             session.chosen, session.key_count, records, 1);
    if (!text || !tune_write_text(session.tune_path, text)) {
      fprintf(stderr, "error: cannot write '%s'\n", session.tune_path);
      tune_restore(session.tune_path, original);
      free(text);
      free(original);
      return 1;
    }
    free(text);
  }

  tune_print_summary(&session);
  printf("tune: wrote %s; rebuild to use it\n", session.tune_path);
  for (int b = 0; b < session.built_count; b++) {
    remove(session.artifacts[b]);
  }
  free(original);
  return 0;
}
