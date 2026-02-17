#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* section: general tools*/
static void die_oom(const char *what) {
  fprintf(stderr, "error: out of memory while %s\n",
          what ? what : "allocating");
  exit(1);
}

static bool starts_with(const char *s, const char *prefix) {
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

static bool ends_with(const char *s, const char *suffix) {
  size_t n1 = strlen(s);
  size_t n2 = strlen(suffix);
  return n1 >= n2 && !strcmp(s + n1 - n2, suffix);
}

typedef struct {
  char **data;
  int len;
  int cap;
} StrVec;

static void strvec_push(StrVec *v, const char *s) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->data = realloc(v->data, (size_t)v->cap * sizeof(*v->data));
    if (!v->data)
      die_oom("growing argv string vector");
  }
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (!copy)
    die_oom("copying argv string");
  memcpy(copy, s, n);
  v->data[v->len++] = copy;
}

/* section: parse arguments */

typedef struct {
  bool dump_tokens;
  bool dump_codegen;
  bool verbose;
  StrVec include_paths;
  StrVec defines;
  StrVec inputs;     // all non-option inputs, in argv order
  StrVec c_inputs;   // *.c
  StrVec asm_inputs; // *.s
  StrVec obj_inputs; // *.o
  StrVec ar_inputs;  // *.a
  StrVec so_inputs;  // *.so
  StrVec other_inputs;
  StrVec ld_args;     // -Wl, -l, -L, -Xlinker ...
  const char *output; // -o <path>
  bool opt_c;
  bool opt_S;
  bool opt_E;
} Options;

static Options opt = {
    .dump_tokens = false,
    .dump_codegen = true,
    .verbose = false,
    .include_paths = {},
    .defines = {},
    .inputs = {},
    .c_inputs = {},
    .asm_inputs = {},
    .obj_inputs = {},
    .ar_inputs = {},
    .so_inputs = {},
    .other_inputs = {},
    .ld_args = {},
    .output = NULL,
    .opt_c = false,
    .opt_S = false,
    .opt_E = false,
};

typedef enum {
  OPT_MATCH_EXACT,
  OPT_MATCH_PREFIX,
} OptMatchKind;

typedef struct {
  const char *names[3];
  const char *help;
  int nargs;
  OptMatchKind match;
  bool (*apply)(Options *opt, int nargs, const char **values);
} OptionSpec;

static void print_help(FILE *out);
static void print_errhint();

static bool opt_set_output(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  opt->output = values[0];
  return true;
}

static bool opt_set_c(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->opt_c = true;
  return true;
}

static bool opt_set_S(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->opt_S = true;
  return true;
}

static bool opt_set_E(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->opt_E = true;
  return true;
}

static bool opt_add_include_path(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  strvec_push(&opt->include_paths, values[0]);
  return true;
}

static bool opt_add_define(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  strvec_push(&opt->defines, values[0]);
  return true;
}

static bool opt_add_ld_arg(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  strvec_push(&opt->ld_args, values[0]);
  return true;
}

static bool opt_add_ld_arg_literal(Options *opt, int nargs,
                                   const char **values) {
  // Pass through the original argv token like "-lfoo".
  if (nargs != 1)
    return false;
  strvec_push(&opt->ld_args, values[0]);
  return true;
}

static bool opt_add_Wl(Options *opt, int nargs, const char **values) {
  // values[0] is a single string like "a,b,c" from "-Wl,a,b,c".
  if (nargs != 1)
    return false;

  char *s = malloc(strlen(values[0]) + 1);
  if (!s)
    die_oom("copying -Wl argument");
  strcpy(s, values[0]);

  for (char *p = strtok(s, ","); p; p = strtok(NULL, ",")) {
    if (*p)
      strvec_push(&opt->ld_args, p);
  }

  free(s);
  return true;
}

static bool opt_set_dump_tokens(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->dump_tokens = true;
  return true;
}

static bool opt_set_no_codegen(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->dump_codegen = false;
  return true;
}

static bool opt_set_verbose(Options *opt, int nargs, const char **values) {
  (void)nargs;
  (void)values;
  opt->verbose = true;
  return true;
}

static bool opt_set_input(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  const char *path = values[0];
  strvec_push(&opt->inputs, path);

  if (ends_with(path, ".c")) {
    strvec_push(&opt->c_inputs, path);
  } else if (ends_with(path, ".s")) {
    strvec_push(&opt->asm_inputs, path);
  } else if (ends_with(path, ".o")) {
    strvec_push(&opt->obj_inputs, path);
  } else if (ends_with(path, ".a")) {
    strvec_push(&opt->ar_inputs, path);
  } else if (ends_with(path, ".so")) {
    strvec_push(&opt->so_inputs, path);
  } else {
    strvec_push(&opt->other_inputs, path);
  }
  return true;
}

static bool opt_help(Options *opt, int nargs, const char **values) {
  (void)opt;
  (void)nargs;
  (void)values;
  print_help(stdout);
  exit(0);
  return true;
}

#define OPT1(name1, helpstr, nargs_, applyfn)                                  \
  {{(name1), NULL, NULL}, (helpstr), (nargs_), OPT_MATCH_EXACT, (applyfn)}

#define OPT2(name1, name2, helpstr, nargs_, applyfn)                           \
  {{(name1), (name2), NULL}, (helpstr), (nargs_), OPT_MATCH_EXACT, (applyfn)}

#define OPT3(name1, name2, name3, helpstr, nargs_, applyfn)                    \
  {{(name1), (name2), (name3)}, (helpstr), (nargs_), OPT_MATCH_EXACT, (applyfn)}

#define OPTP1(prefix, helpstr, nargs_, applyfn)                                \
  {{(prefix), NULL, NULL}, (helpstr), (nargs_), OPT_MATCH_PREFIX, (applyfn)}

static const OptionSpec specs[] = {
    OPTP1("-o", "set output path", 1, opt_set_output),
    OPT1("-c", "compile and assemble, but do not link", 0, opt_set_c),
    OPT1("-S", "compile only; do not assemble or link", 0, opt_set_S),
    OPT1("-E", "preprocess only", 0, opt_set_E),
    OPTP1("-I", "add include search path", 1, opt_add_include_path),
    OPTP1("-D", "define macro (NAME or NAME=VALUE)", 1, opt_add_define),
    OPTP1("-Wl", "pass comma-separated args to linker", 1, opt_add_Wl),
    OPTP1("-l", "link with library (pass through to linker)", 1,
          opt_add_ld_arg_literal),
    OPTP1("-L", "add linker search path", 1, opt_add_ld_arg),
    OPT1("-Xlinker", "pass one argument to linker", 1, opt_add_ld_arg),
    OPT2("-h", "--help", "show this help", 0, opt_help),
    OPT1("--tokens", "dump tokens then continue", 0, opt_set_dump_tokens),
    OPT1("--no-codegen", "parse only; do not emit code", 0, opt_set_no_codegen),
    OPT1("--verbose", "print parsed options", 0, opt_set_verbose),
};

static const size_t specs_len = sizeof(specs) / sizeof(*specs);

static void print_help(FILE *out) {
  fprintf(out, "usage: feipiaocc <file> [options]\n");
  fprintf(out, "options:\n");
  for (size_t i = 0; i < specs_len; i++) {
    const OptionSpec *s = &specs[i];
    fprintf(out, "  ");
    for (size_t j = 0; j < sizeof(s->names) / sizeof(*s->names); j++) {
      const char *name = s->names[j];
      if (!name)
        break;
      if (j != 0)
        fprintf(out, ", ");
      fprintf(out, "%s", name);
    }
    fprintf(out, "%s%s\n", s->help ? "\t" : "", s->help ? s->help : "");
  }
}

static void print_errhint() { fprintf(stderr, "hint: try --help\n"); }

static bool match_option(const OptionSpec *spec, const char *arg,
                         const char **inline_value) {
  *inline_value = NULL;

  for (size_t i = 0; i < sizeof(spec->names) / sizeof(*spec->names); i++) {
    const char *name = spec->names[i];
    if (!name)
      break;

    if (spec->match == OPT_MATCH_EXACT) {
      if (!strcmp(arg, name))
        return true;
      continue;
    }

    // OPT_MATCH_PREFIX
    if (!strcmp(arg, name))
      return true;

    if (starts_with(arg, name)) {
      const char *suffix = arg + strlen(name);
      if (*suffix != '\0') {
        *inline_value = suffix;
        return true;
      }
    }
  }
  return false;
}

#define ERRORF(...)                                                            \
  do {                                                                         \
    fprintf(stderr, "error: ");                                                \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
  } while (0)

#define INNER_ERRORF(...)                                                      \
  do {                                                                         \
    fprintf(stderr, "inner error: ");                                          \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
  } while (0)

#define DIE(...)                                                               \
  do {                                                                         \
    ERRORF(__VA_ARGS__);                                                       \
    exit(1);                                                                   \
  } while (0)

#define INNER_DIE(...)                                                         \
  do {                                                                         \
    INNER_ERRORF(__VA_ARGS__);                                                 \
    exit(1);                                                                   \
  } while (0)

#define DIE_HINT(...)                                                          \
  do {                                                                         \
    ERRORF(__VA_ARGS__);                                                       \
    print_errhint();                                                           \
    exit(1);                                                                   \
  } while (0)

void parse_argv(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      print_help(stdout);
      exit(0);
    }
  }

  bool stop_options = false;

  for (int i = 1; i < argc; i++) {
    char *arg = argv[i];

    if (!stop_options && !strcmp(arg, "--")) {
      stop_options = true;
      continue;
    }

    if (!stop_options && arg[0] == '-' && arg[1] != '\0') {
      const OptionSpec *spec = NULL;
      const char *inline_value = NULL;

      for (size_t j = 0; j < specs_len; j++) {
        const char *v = NULL;
        if (match_option(&specs[j], arg, &v)) {
          spec = &specs[j];
          inline_value = v;
          break;
        }
      }

      if (!spec) {
        DIE_HINT("unknown option: %s", arg);
      }

      const char **values = NULL;
      if (spec->nargs < 0)
        INNER_DIE("invalid nargs=%d for %s", spec->nargs, arg);

      int remaining = spec->nargs;
      if (inline_value)
        remaining--;
      if (remaining < 0)
        INNER_DIE("invalid nargs=%d for %s", spec->nargs, arg);

      if (spec->nargs != 0) {
        values = calloc((size_t)spec->nargs, sizeof(*values));
        int idx = 0;
        if (inline_value)
          values[idx++] = inline_value;

        for (int n = 0; n < remaining; n++) {
          if (i + 1 >= argc)
            DIE("option requires %d argument(s): %s", spec->nargs, arg);

          char *v = argv[i + 1];
          if (v[0] == '-' && v[1] != '\0')
            DIE("option requires %d argument(s): %s", spec->nargs, arg);
          values[idx++] = argv[++i];
        }
      }

      bool ok = spec->apply(&opt, spec->nargs, values);
      free(values);
      if (!ok)
        DIE("failed to apply option: %s", arg);
      continue;
    }

    const char *v[] = {arg};
    if (!opt_set_input(&opt, 1, v))
      DIE("failed to set input file: %s", arg);
  }
}

static void dump_options(FILE *out, const Options *opt) {
  fprintf(out, "verbose: %s\n", opt->verbose ? "true" : "false");
  fprintf(out, "dump_tokens: %s\n", opt->dump_tokens ? "true" : "false");
  fprintf(out, "dump_codegen: %s\n", opt->dump_codegen ? "true" : "false");
  fprintf(out, "opt_c: %s\n", opt->opt_c ? "true" : "false");
  fprintf(out, "opt_S: %s\n", opt->opt_S ? "true" : "false");
  fprintf(out, "opt_E: %s\n", opt->opt_E ? "true" : "false");
  fprintf(out, "output: %s\n", opt->output ? opt->output : "(null)");

  fprintf(out, "include_paths(%d):\n", opt->include_paths.len);
  for (int i = 0; i < opt->include_paths.len; i++)
    fprintf(out, "  %s\n", opt->include_paths.data[i]);

  fprintf(out, "defines(%d):\n", opt->defines.len);
  for (int i = 0; i < opt->defines.len; i++)
    fprintf(out, "  %s\n", opt->defines.data[i]);

  fprintf(out, "ld_args(%d):\n", opt->ld_args.len);
  for (int i = 0; i < opt->ld_args.len; i++)
    fprintf(out, "  %s\n", opt->ld_args.data[i]);

  fprintf(out, "inputs(%d):\n", opt->inputs.len);
  for (int i = 0; i < opt->inputs.len; i++)
    fprintf(out, "  %s\n", opt->inputs.data[i]);

  fprintf(out, "c_inputs(%d):\n", opt->c_inputs.len);
  for (int i = 0; i < opt->c_inputs.len; i++)
    fprintf(out, "  %s\n", opt->c_inputs.data[i]);

  fprintf(out, "asm_inputs(%d):\n", opt->asm_inputs.len);
  for (int i = 0; i < opt->asm_inputs.len; i++)
    fprintf(out, "  %s\n", opt->asm_inputs.data[i]);

  fprintf(out, "obj_inputs(%d):\n", opt->obj_inputs.len);
  for (int i = 0; i < opt->obj_inputs.len; i++)
    fprintf(out, "  %s\n", opt->obj_inputs.data[i]);

  fprintf(out, "ar_inputs(%d):\n", opt->ar_inputs.len);
  for (int i = 0; i < opt->ar_inputs.len; i++)
    fprintf(out, "  %s\n", opt->ar_inputs.data[i]);

  fprintf(out, "so_inputs(%d):\n", opt->so_inputs.len);
  for (int i = 0; i < opt->so_inputs.len; i++)
    fprintf(out, "  %s\n", opt->so_inputs.data[i]);

  fprintf(out, "other_inputs(%d):\n", opt->other_inputs.len);
  for (int i = 0; i < opt->other_inputs.len; i++)
    fprintf(out, "  %s\n", opt->other_inputs.data[i]);
}

static void validate_options(const Options *opt) {
  if (opt->opt_E && (opt->opt_c || opt->opt_S))
    DIE_HINT("conflicting options: -E cannot be used with -c or -S");

  if (opt->opt_c && opt->opt_S)
    DIE_HINT("conflicting options: -c cannot be used with -S");

  // For per-input outputs (-E/-S/-c), using a single -o with multiple inputs
  // is ambiguous. GCC/clang reject it, and so does chibicc.
  if (opt->output && opt->inputs.len > 1 &&
      (opt->opt_E || opt->opt_S || opt->opt_c))
    DIE_HINT("cannot specify -o with -E, -S or -c when multiple input files "
             "are given");
}

/* section: preprocess part */

// preprocessing-token tokenizer (C11 6.4 / Annex A.1)
//
// Returned tokens are:
//   identifier, pp-number, character-constant, string-literal, punctuator,
//   other(non-white-space)
//
// In addition, we expose NEWLINE/EOF tokens to make directive parsing easier.
//
// Whitespace/comments handling policy (important for directive parsing):
// - Spaces/tabs/etc (excluding '\n') are skipped and recorded via
// `has_space=true`
//   on the *next* returned non-NEWLINE token.
// - `//` comments are skipped up to (but not including) the terminating '\n'.
// - `/* ... */` comments are skipped, but NEWLINE tokens are still produced for
//   newlines that occur inside the block comment. This keeps `at_bol` accurate
//   and allows `#...` directives to be recognized correctly.
// - `has_space` is treated as a boolean ("was there whitespace/comment before
//   this token"), not as an exact count.

typedef enum {
  PPTOK_EOF,
  PPTOK_NEWLINE,
  PPTOK_IDENTIFIER,
  PPTOK_PP_NUMBER,
  PPTOK_CHARACTER_CONSTANT,
  PPTOK_STRING_LITERAL,
  PPTOK_PUNCTUATOR,
  PPTOK_OTHER,
} PPTokenKind;

typedef struct {
  const char *path;
  char *path_buf; // owned copy of path (or NULL if not owned)
  char *contents; // NUL-terminated, normalized to '\n'
} PPFile;

typedef struct PPToken PPToken;

// Source location for preprocessing tokens (used for diagnostics and macro
// backtraces).
typedef struct {
  const char *path;
  long byte_offset; // from file->contents
  int line_no;      // 1-based
  int col_no;       // 1-based
} PPSrcLoc;

typedef struct PPOrigin PPOrigin;
struct PPOrigin {
  const char *macro_name;
  PPSrcLoc expanded_at; // macro invocation site
  PPSrcLoc defined_at;  // macro definition site (optional)
  PPOrigin *parent;     // next frame (outer expansion / original origin)
};

typedef struct PPHideSet PPHideSet;
struct PPHideSet {
  const char *name; // interned pointer
  PPHideSet *next;
};

struct PPToken {
  PPTokenKind kind;
  unsigned id; // monotonically increasing within a translation unit
  const char *loc;
  int len;
  bool at_bol;
  bool has_space;
  PPSrcLoc spelling; // where the token's text is spelled
  PPOrigin *origin;  // macro expansion backtrace (owned by this token)
  PPHideSet *hideset;
  PPToken *next;
};

typedef enum {
  PP_COMMENT_NONE,
  PP_COMMENT_BLOCK,
} PPCommentMode;

typedef struct {
  PPFile *file;
  const char *cur;
  int line_no;
  const char *line_start;
  bool at_bol;
  bool has_space;
  PPCommentMode comment_mode;
  unsigned next_tok_id;
} PPTokenizer;

static PPFile pp_read_file(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    DIE("cannot open file: %s", path);

  if (fseek(fp, 0, SEEK_END) != 0)
    DIE("fseek failed: %s", path);
  long size = ftell(fp);
  if (size < 0)
    DIE("ftell failed: %s", path);
  if (fseek(fp, 0, SEEK_SET) != 0)
    DIE("fseek failed: %s", path);

  // We add a small NUL padding so tokenizer helpers can safely look ahead
  // (e.g. universal-character-name needs up to 10 bytes: "\\UXXXXXXXX")
  // without risking out-of-bounds reads near end-of-file.
  enum { PP_FILE_PADDING = 16 };
  char *buf = malloc((size_t)size + 2 + PP_FILE_PADDING);
  if (!buf)
    die_oom("reading file");

  size_t nread = fread(buf, 1, (size_t)size, fp);
  fclose(fp);

  // Ensure file ends with '\n' (helps diagnostics and NEWLINE tokenization).
  if (nread == 0 || buf[nread - 1] != '\n')
    buf[nread++] = '\n';
  buf[nread] = '\0';

  // Normalize CRLF to LF and drop stray CR.
  {
    size_t w = 0;
    for (size_t r = 0; r < nread; r++) {
      if (buf[r] == '\r')
        continue;
      buf[w++] = buf[r];
    }
    buf[w] = '\0';
    nread = w;
  }

  // Splice backslash-newline (translation phase 2).
  {
    size_t w = 0;
    for (size_t r = 0; r < nread; r++) {
      if (buf[r] == '\\' && r + 1 < nread && buf[r + 1] == '\n') {
        r++;
        continue;
      }
      buf[w++] = buf[r];
    }
    nread = w;
  }

  // Ensure file ends with '\n' after splicing as well.
  if (nread == 0 || buf[nread - 1] != '\n')
    buf[nread++] = '\n';

  buf[nread] = '\0';
  memset(buf + nread + 1, 0, PP_FILE_PADDING);

  size_t pn = strlen(path) + 1;
  char *path_buf = malloc(pn);
  if (!path_buf)
    die_oom("copying file path");
  memcpy(path_buf, path, pn);

  PPFile f = {.path = path_buf, .path_buf = path_buf, .contents = buf};
  return f;
}

static void pp_free_file(PPFile *file) {
  free(file->path_buf);
  free(file->contents);
}

static void pp_tokenizer_init(PPTokenizer *tz, PPFile *file) {
  *tz = (PPTokenizer){
      .file = file,
      .cur = file->contents,
      .line_no = 1,
      .line_start = file->contents,
      .at_bol = true,
      .has_space = false,
      .comment_mode = PP_COMMENT_NONE,
      .next_tok_id = 1,
  };
}

static PPSrcLoc pp_make_srcloc(PPTokenizer *tz, const char *p) {
  return (PPSrcLoc){
      .path = tz->file->path,
      .byte_offset = (long)(p - tz->file->contents),
      .line_no = tz->line_no,
      .col_no = (int)(p - tz->line_start) + 1,
  };
}

static bool pp_srcloc_is_valid(PPSrcLoc loc) {
  return loc.path && loc.line_no > 0 && loc.col_no > 0;
}

static void pp_fprint_srcloc(FILE *out, PPSrcLoc loc) {
  if (!pp_srcloc_is_valid(loc)) {
    fprintf(out, "<unknown>");
    return;
  }
  fprintf(out, "%s:%d:%d", loc.path, loc.line_no, loc.col_no);
}

static void pp_origin_free(PPOrigin *o) {
  while (o) {
    PPOrigin *next = o->parent;
    free(o);
    o = next;
  }
}

static void pp_hideset_free(PPHideSet *hs) {
  while (hs) {
    PPHideSet *next = hs->next;
    free(hs);
    hs = next;
  }
}

static bool pp_is_space_non_nl(int c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

static bool pp_is_nondigit(int c) {
  return isalpha((unsigned char)c) || c == '_';
}

static bool pp_is_digit(int c) { return isdigit((unsigned char)c); }

static bool pp_is_xdigit(int c) { return isxdigit((unsigned char)c); }

static int pp_scan_ucn_len(const char *p) {
  // universal-character-name: \uXXXX or \UXXXXXXXX
  if (p[0] != '\\')
    return 0;
  if (p[1] == 'u') {
    for (int i = 0; i < 4; i++)
      if (!pp_is_xdigit((unsigned char)p[2 + i]))
        return 0;
    return 6;
  }
  if (p[1] == 'U') {
    for (int i = 0; i < 8; i++)
      if (!pp_is_xdigit((unsigned char)p[2 + i]))
        return 0;
    return 10;
  }
  return 0;
}

static bool pp_is_ident1(const char *p) {
  return pp_is_nondigit((unsigned char)p[0]) || pp_scan_ucn_len(p) != 0;
}

static bool pp_is_punctuator_first(int c) {
  // See C11 Annex A.1.7. We only need a conservative "can start punctuator"
  // set.
  switch (c) {
  case '[':
  case ']':
  case '(':
  case ')':
  case '{':
  case '}':
  case '.':
  case '&':
  case '*':
  case '+':
  case '-':
  case '~':
  case '!':
  case '/':
  case '%':
  case '<':
  case '>':
  case '^':
  case '|':
  case '?':
  case ':':
  case ';':
  case '=':
  case ',':
  case '#':
    return true;
  default:
    return false;
  }
}

static bool pp_is_string_or_char_start(const char *p, int *prefix_len_out,
                                       char *quote_out) {
  // Recognize starts of:
  // - string-literal: encoding-prefix(opt) "..."
  // - character-constant: (L|u|U)(opt) '...'
  //
  // This helper only identifies (prefix_len, quote) and does not advance any
  // tokenizer state. The caller scans the body.
  if (p[0] == '"' || p[0] == '\'') {
    *prefix_len_out = 0;
    *quote_out = p[0];
    return true;
  }

  // u8"..." or u8'...'
  if (p[0] == 'u' && p[1] == '8' && (p[2] == '"' || p[2] == '\'')) {
    *prefix_len_out = 2;
    *quote_out = p[2];
    return true;
  }

  // u"..." / u'...' / U"..." / U'...' / L"..." / L'...'
  if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') &&
      (p[1] == '"' || p[1] == '\'')) {
    *prefix_len_out = 1;
    *quote_out = p[1];
    return true;
  }

  return false;
}

static int pp_read_punct_len(const char *p) {
  // Maximal munch for punctuators (C11 6.4.6).
  static const char *ops[] = {
      "%:%:", "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=",
      ">=",   "==",  "!=",  "&&",  "||", "*=", "/=", "%=", "+=", "-=",
      "&=",   "^=",  "|=",  "##",  "<:", ":>", "<%", "%>", "%:",
  };

  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++) {
    size_t n = strlen(ops[i]);
    if (!strncmp(p, ops[i], n))
      return (int)n;
  }
  return pp_is_punctuator_first((unsigned char)p[0]) ? 1 : 0;
}

static const char *pp_tok_kind_name(PPTokenKind k) {
  switch (k) {
  case PPTOK_EOF:
    return "EOF";
  case PPTOK_NEWLINE:
    return "NEWLINE";
  case PPTOK_IDENTIFIER:
    return "IDENTIFIER";
  case PPTOK_PP_NUMBER:
    return "PP_NUMBER";
  case PPTOK_CHARACTER_CONSTANT:
    return "CHARACTER_CONSTANT";
  case PPTOK_STRING_LITERAL:
    return "STRING_LITERAL";
  case PPTOK_PUNCTUATOR:
    return "PUNCTUATOR";
  case PPTOK_OTHER:
    return "OTHER";
  }
  return "UNKNOWN";
}

static PPToken pp_make_tok(PPTokenizer *tz, PPTokenKind kind, const char *start,
                           const char *end, bool at_bol, bool has_space) {
  return (PPToken){
      .kind = kind,
      .id = tz->next_tok_id++,
      .loc = start,
      .len = (int)(end - start),
      .at_bol = at_bol,
      .has_space = has_space,
      .spelling = pp_make_srcloc(tz, start),
      .origin = NULL,
      .hideset = NULL,
      .next = NULL,
  };
}

static bool pp_try_quoted_literal_end(PPTokenizer *tz, const char *p,
                                      char quote, const char **end_out) {
  // p points to the first character after opening quote.
  while (*p && *p != quote) {
    if (*p == '\\' && p[1]) {
      p += 2;
      continue;
    }
    if (*p == '\n')
      DIE("%s:%d: unclosed string/char literal", tz->file->path, tz->line_no);
    p++;
  }
  if (*p != quote)
    DIE("%s:%d: unclosed string/char literal", tz->file->path, tz->line_no);
  *end_out = p + 1;
  return true;
}

static bool pp_try_pp_number_end(const char *p, const char **end_out) {
  if (!(pp_is_digit((unsigned char)*p) ||
        (*p == '.' && pp_is_digit((unsigned char)p[1]))))
    // only punctuator and pp_number has "."
    return false;

  // C11 6.4.8: pp-number
  const char *q = p + 1; // consumed first digit or '.'
  for (;;) {
    if (isalnum((unsigned char)*q) || *q == '.' || *q == '_') {
      q++;
      continue;
    }

    // 1e+2, 0x1.2p-3
    if ((*q == '+' || *q == '-') &&
        (q[-1] == 'e' || q[-1] == 'E' || q[-1] == 'p' || q[-1] == 'P')) {
      q++;
      continue;
    }

    int ucn_len = pp_scan_ucn_len(q);
    if (ucn_len) {
      q += ucn_len;
      continue;
    }

    break;
  }

  *end_out = q;
  return true;
}

static bool pp_try_identifier_end(const char *p, const char **end_out) {
  if (!pp_is_ident1(p))
    return false;

  const char *q = p;

  int ucn_len = pp_scan_ucn_len(q);
  if (ucn_len)
    q += ucn_len;
  else
    q++;

  while (*q) {
    ucn_len = pp_scan_ucn_len(q);
    if (ucn_len) {
      q += ucn_len;
      continue;
    }

    if (!isalnum((unsigned char)*q) && *q != '_')
      break;
    q++;
  }

  *end_out = q;
  return true;
}

static bool pp_try_string_or_char(PPTokenizer *tz, const char *p,
                                  bool tok_at_bol, bool tok_has_space,
                                  PPToken *out) {
  int prefix_len = 0;
  char quote = '\0';
  if (!pp_is_string_or_char_start(p, &prefix_len, &quote))
    return false;

  const char *start = p;
  const char *q = p + prefix_len + 1;
  const char *end = NULL;
  (void)pp_try_quoted_literal_end(tz, q, quote, &end);

  PPTokenKind kind =
      (quote == '"') ? PPTOK_STRING_LITERAL : PPTOK_CHARACTER_CONSTANT;
  *out = pp_make_tok(tz, kind, start, end, tok_at_bol, tok_has_space);
  tz->cur = end;
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_pp_number(PPTokenizer *tz, const char *p, bool tok_at_bol,
                             bool tok_has_space, PPToken *out) {
  if (!(pp_is_digit((unsigned char)*p) ||
        (*p == '.' && pp_is_digit((unsigned char)p[1]))))
    return false;

  const char *start = p;
  const char *end = NULL;
  if (!pp_try_pp_number_end(p, &end))
    return false;
  *out =
      pp_make_tok(tz, PPTOK_PP_NUMBER, start, end, tok_at_bol, tok_has_space);
  tz->cur = end;
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_identifier(PPTokenizer *tz, const char *p, bool tok_at_bol,
                              bool tok_has_space, PPToken *out) {
  if (!pp_is_ident1(p))
    return false;

  const char *start = p;
  const char *end = NULL;
  if (!pp_try_identifier_end(p, &end))
    return false;
  *out =
      pp_make_tok(tz, PPTOK_IDENTIFIER, start, end, tok_at_bol, tok_has_space);
  tz->cur = end;
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_punctuator(PPTokenizer *tz, const char *p, bool tok_at_bol,
                              bool tok_has_space, PPToken *out) {
  if (!pp_is_punctuator_first((unsigned char)*p))
    return false;

  int n = pp_read_punct_len(p);
  const char *end = p + n;
  *out = pp_make_tok(tz, PPTOK_PUNCTUATOR, p, end, tok_at_bol, tok_has_space);
  tz->cur = end;
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_newline(PPTokenizer *tz, const char *p, PPToken *out) {
  if (*p != '\n')
    return false;

  *out = pp_make_tok(tz, PPTOK_NEWLINE, p, p + 1, tz->at_bol, tz->has_space);
  tz->cur = p + 1;
  tz->line_no++;
  tz->line_start = tz->cur;
  tz->at_bol = true;
  tz->has_space = false;
  return true;
}

static bool pp_try_eof(PPTokenizer *tz, const char *p, PPToken *out) {
  if (*p != '\0')
    return false;

  *out = pp_make_tok(tz, PPTOK_EOF, p, p, tz->at_bol, tz->has_space);
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_skip_spaces(PPTokenizer *tz, const char *p) {
  if (!pp_is_space_non_nl((unsigned char)*p))
    return false;

  while (pp_is_space_non_nl((unsigned char)*p)) {
    tz->has_space = true;
    p++;
  }
  tz->cur = p;
  return true;
}

static bool pp_try_skip_line_comment(PPTokenizer *tz, const char *p) {
  if (!(p[0] == '/' && p[1] == '/'))
    return false;

  // Line comments count as whitespace. We stop at '\n' so the newline can be
  // returned as a NEWLINE token by pp_try_newline().
  tz->has_space = true;
  p += 2;
  while (*p && *p != '\n')
    p++;
  tz->cur = p; // leave '\n' to pp_try_newline
  return true;
}

static bool pp_try_enter_block_comment(PPTokenizer *tz, const char *p) {
  if (!(p[0] == '/' && p[1] == '*'))
    return false;

  // Block comments count as whitespace. We switch to PP_COMMENT_BLOCK mode and
  // let pp_try_in_block_comment() drive scanning. That function still returns
  // NEWLINE tokens for any '\n' encountered inside the comment.
  tz->has_space = true;
  tz->comment_mode = PP_COMMENT_BLOCK;
  tz->cur = p + 2;
  return true;
}

static bool pp_try_in_block_comment(PPTokenizer *tz, PPToken *out) {
  if (tz->comment_mode != PP_COMMENT_BLOCK)
    return false;

  // We're inside a /* ... */ comment. We must:
  // - detect the closing "*/" and exit comment mode
  // - produce NEWLINE tokens for '\n' encountered while skipping the comment
  // - error out at EOF if the comment is unterminated
  //
  // Standard mapping:
  // - Comments are removed before tokenization: C11 5.1.1.2 translation phase 3
  //   replaces each comment with a single space.
  // - We still surface '\n' as PPTOK_NEWLINE to support directive parsing in
  //   our preprocessor implementation (directives are line-based; see
  //   C11 6.10).
  const char *p = tz->cur;

  for (;;) {
    if (*p == '\0')
      DIE("%s:%d: unclosed block comment", tz->file->path, tz->line_no);

    if (p[0] == '*' && p[1] == '/') {
      tz->comment_mode = PP_COMMENT_NONE;
      tz->has_space = true;
      tz->cur = p + 2;
      return false; // no token produced; continue scanning normally
    }

    if (*p == '\n') {
      *out =
          pp_make_tok(tz, PPTOK_NEWLINE, p, p + 1, tz->at_bol, tz->has_space);
      tz->cur = p + 1;
      tz->line_no++;
      tz->line_start = tz->cur;
      tz->at_bol = true;
      tz->has_space = false;
      return true;
    }

    p++;
    tz->cur = p;
  }
}

static bool pp_try_next_preprocessing_token(PPTokenizer *tz, PPToken *out) {
  for (;;) {
    const char *p = tz->cur;

    // Drive block comment skipping. (Not part of the lexical grammar; comments
    // are removed in translation phase 3. See C11 5.1.1.2.)
    if (pp_try_in_block_comment(tz, out))
      return true;

    p = tz->cur;
    // Return NEWLINE tokens to make directive parsing (C11 6.10) simpler.
    if (pp_try_newline(tz, p, out))
      return true;

    p = tz->cur;
    // Skip whitespace (excluding '\n') and remember it via has_space.
    if (pp_try_skip_spaces(tz, p))
      continue;

    p = tz->cur;
    // Skip // comments up to '\n'. (Comments removed in translation phase 3.)
    if (pp_try_skip_line_comment(tz, p))
      continue;

    p = tz->cur;
    // Enter /* */ comment mode. (Comments removed in translation phase 3.)
    if (pp_try_enter_block_comment(tz, p))
      continue;

    p = tz->cur;
    // EOF token (not in C11 preprocessing-token; exposed for implementation).
    if (pp_try_eof(tz, p, out))
      return true;

    bool tok_at_bol = tz->at_bol;
    bool tok_has_space = tz->has_space;

    // string-literal / character-constant (C11 6.4.5 / 6.4.4.4).
    if (pp_try_string_or_char(tz, p, tok_at_bol, tok_has_space, out))
      return true;

    // pp-number (C11 6.4.8).
    // only punctuator and pp_number has "."
    // here we do not match them together, so we need match pp_number first
    if (pp_try_pp_number(tz, p, tok_at_bol, tok_has_space, out))
      return true;

    // identifier (C11 6.4.2.1).
    if (pp_try_identifier(tz, p, tok_at_bol, tok_has_space, out))
      return true;

    // punctuator (C11 6.4.6, maximal munch).
    if (pp_try_punctuator(tz, p, tok_at_bol, tok_has_space, out))
      return true;

    // other (any non-whitespace character)
    // C11 6.4 preprocessing-token: "each non-white-space character that cannot
    // be one of the above".
    tz->cur = p + 1;
    tz->at_bol = false;
    tz->has_space = false;
    *out = pp_make_tok(tz, PPTOK_OTHER, p, p + 1, tok_at_bol, tok_has_space);
    return true;
  }
}

static PPToken next_preprocessing_token(PPTokenizer *tz) {
  PPToken tok = {};
  if (!pp_try_next_preprocessing_token(tz, &tok))
    INNER_DIE("pp_try_next_preprocessing_token failed");
  return tok;
}

// Tokenize a whole source file into a PPToken linked list.
//
// Note: returned tokens' `loc/len` slices point into `file->contents`, so the
// caller must keep `file->contents` alive for as long as the list is used.
static PPToken *tokenlize(PPFile *file) {
  PPTokenizer tz;
  pp_tokenizer_init(&tz, file);

  PPToken head = {};
  PPToken *cur = &head;

  for (;;) {
    PPToken tok = next_preprocessing_token(&tz);
    PPToken *node = calloc(1, sizeof(*node));
    if (!node)
      die_oom("allocating preprocessing token");
    *node = tok;
    node->next = NULL;
    cur = cur->next = node;

    if (tok.kind == PPTOK_EOF)
      break;
  }

  return head.next;
}

static void free_pptokens(PPToken *tok) {
  while (tok) {
    PPToken *next = tok->next;
    pp_origin_free(tok->origin);
    pp_hideset_free(tok->hideset);
    free(tok);
    tok = next;
  }
}

static void pp_free_tok(PPToken *tok) {
  if (!tok)
    return;
  pp_origin_free(tok->origin);
  pp_hideset_free(tok->hideset);
  free(tok);
}

static bool pp_tok_text_is(PPToken *tok, const char *s) {
  if (!tok || !s)
    return false;
  size_t n = strlen(s);
  return tok->len == (int)n && !memcmp(tok->loc, s, n);
}

static bool pp_is_directive_start(PPToken *tok) {
  // Directives are recognized only at the beginning of a logical line (after
  // optional whitespace). Our tokenizer removes whitespace tokens; if a line
  // begins with spaces, the '#' token still has at_bol=true and has_space=true.
  return tok && tok->at_bol && tok->kind == PPTOK_PUNCTUATOR &&
         (pp_tok_text_is(tok, "#") || pp_tok_text_is(tok, "%:"));
}

static PPToken *pp_skip_to_line_end(PPToken *tok) {
  while (tok && tok->kind != PPTOK_EOF && tok->kind != PPTOK_NEWLINE)
    tok = tok->next;
  if (tok && tok->kind == PPTOK_NEWLINE)
    tok = tok->next;
  return tok;
}

static PPOrigin *pp_origin_clone(const PPOrigin *o) {
  if (!o)
    return NULL;

  PPOrigin *head = NULL;
  PPOrigin **tail = &head;
  for (const PPOrigin *p = o; p; p = p->parent) {
    PPOrigin *node = calloc(1, sizeof(*node));
    if (!node)
      die_oom("allocating macro origin");
    *node = *p;
    node->parent = NULL;
    *tail = node;
    tail = &node->parent;
  }
  return head;
}

static PPHideSet *pp_hideset_clone(const PPHideSet *hs) {
  if (!hs)
    return NULL;
  PPHideSet *head = NULL;
  PPHideSet **tail = &head;
  for (const PPHideSet *p = hs; p; p = p->next) {
    PPHideSet *node = calloc(1, sizeof(*node));
    if (!node)
      die_oom("allocating hideset");
    *node = *p;
    node->next = NULL;
    *tail = node;
    tail = &node->next;
  }
  return head;
}

static PPToken *pp_clone_tok(const PPToken *tok) {
  PPToken *node = calloc(1, sizeof(*node));
  if (!node)
    die_oom("allocating preprocessing token");
  *node = *tok;
  node->next = NULL;
  node->origin = pp_origin_clone(tok->origin);
  node->hideset = pp_hideset_clone(tok->hideset);
  return node;
}

static void pp_die_tok(PPToken *tok, const char *msg) {
  if (tok)
    DIE("%s:%d:%d: %s", tok->spelling.path, tok->spelling.line_no,
        tok->spelling.col_no, msg);
  DIE("<unknown>:0:0: %s", msg);
}

static bool pp_is_empty_directive(PPToken *hash_tok) {
  if (!pp_is_directive_start(hash_tok))
    return false;
  return !hash_tok->next || hash_tok->next->kind == PPTOK_NEWLINE ||
         hash_tok->next->kind == PPTOK_EOF;
}

static PPToken *pp_directive_name(PPToken *hash_tok) {
  if (!pp_is_directive_start(hash_tok))
    return NULL;
  PPToken *p = hash_tok->next;
  if (!p || p->kind == PPTOK_NEWLINE || p->kind == PPTOK_EOF)
    return NULL;
  if (p->kind != PPTOK_IDENTIFIER)
    return NULL;
  return p;
}

static bool pp_directive_is(PPToken *hash_tok, const char *name) {
  PPToken *p = pp_directive_name(hash_tok);
  return p && pp_tok_text_is(p, name);
}

static bool pp_is_endif_like(PPToken *hash_tok) {
  return pp_directive_is(hash_tok, "elif") || pp_directive_is(hash_tok, "else") ||
         pp_directive_is(hash_tok, "endif");
}

static char *pp_strndup(const char *p, int len) {
  char *s = malloc((size_t)len + 1);
  if (!s)
    die_oom("copying string");
  memcpy(s, p, (size_t)len);
  s[len] = '\0';
  return s;
}

static bool pp_hideset_contains(const PPHideSet *hs, const char *s, int len) {
  for (const PPHideSet *p = hs; p; p = p->next) {
    if ((int)strlen(p->name) == len && !strncmp(p->name, s, (size_t)len))
      return true;
  }
  return false;
}

static PPHideSet *pp_hideset_add_name(PPHideSet *hs, const char *name) {
  int len = (int)strlen(name);
  if (pp_hideset_contains(hs, name, len))
    return hs;
  PPHideSet *node = calloc(1, sizeof(*node));
  if (!node)
    die_oom("allocating hideset");
  node->name = name;
  node->next = hs;
  return node;
}

static PPHideSet *pp_hideset_union(const PPHideSet *a, const PPHideSet *b) {
  PPHideSet *out = NULL;
  for (const PPHideSet *p = a; p; p = p->next)
    out = pp_hideset_add_name(out, p->name);
  for (const PPHideSet *p = b; p; p = p->next)
    out = pp_hideset_add_name(out, p->name);
  return out;
}

typedef struct {
  char *key;
  int keylen;
  void *val;
} PPHashEntry;

typedef struct {
  PPHashEntry *buckets;
  int capacity;
  int used;
} PPHashMap;

#define PP_TOMBSTONE ((void *)-1)

static uint64_t pp_fnv_hash(char *s, int len) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (int i = 0; i < len; i++) {
    hash *= 0x100000001b3ULL;
    hash ^= (unsigned char)s[i];
  }
  return hash;
}

static bool pp_hash_match(PPHashEntry *ent, char *key, int keylen) {
  return ent->key && ent->key != (char *)PP_TOMBSTONE && ent->keylen == keylen &&
         !memcmp(ent->key, key, (size_t)keylen);
}

static void pp_hash_rehash(PPHashMap *map) {
  int nkeys = 0;
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != (char *)PP_TOMBSTONE)
      nkeys++;

  int cap = map->capacity ? map->capacity : 16;
  while ((nkeys * 100) / cap >= 50)
    cap *= 2;

  PPHashMap map2 = {};
  map2.capacity = cap;
  map2.buckets = calloc((size_t)cap, sizeof(PPHashEntry));
  if (!map2.buckets)
    die_oom("allocating hashmap");

  for (int i = 0; i < map->capacity; i++) {
    PPHashEntry *ent = &map->buckets[i];
    if (ent->key && ent->key != (char *)PP_TOMBSTONE) {
      // re-insert
      uint64_t hash = pp_fnv_hash(ent->key, ent->keylen);
      for (int j = 0; j < map2.capacity; j++) {
        PPHashEntry *e2 = &map2.buckets[(hash + (uint64_t)j) % (uint64_t)map2.capacity];
        if (!e2->key) {
          *e2 = *ent;
          map2.used++;
          break;
        }
      }
    }
  }

  free(map->buckets);
  *map = map2;
}

static PPHashEntry *pp_hash_get_entry(PPHashMap *map, char *key, int keylen) {
  if (!map->buckets)
    return NULL;
  uint64_t hash = pp_fnv_hash(key, keylen);
  for (int i = 0; i < map->capacity; i++) {
    PPHashEntry *ent =
        &map->buckets[(hash + (uint64_t)i) % (uint64_t)map->capacity];
    if (pp_hash_match(ent, key, keylen))
      return ent;
    if (ent->key == NULL)
      return NULL;
  }
  INNER_DIE("unreachable: pp_hash_get_entry");
}

static PPHashEntry *pp_hash_get_or_insert(PPHashMap *map, char *key, int keylen) {
  if (!map->buckets) {
    map->capacity = 16;
    map->buckets = calloc((size_t)map->capacity, sizeof(PPHashEntry));
    if (!map->buckets)
      die_oom("allocating hashmap");
  } else if ((map->used * 100) / map->capacity >= 70) {
    pp_hash_rehash(map);
  }

  uint64_t hash = pp_fnv_hash(key, keylen);
  for (int i = 0; i < map->capacity; i++) {
    PPHashEntry *ent =
        &map->buckets[(hash + (uint64_t)i) % (uint64_t)map->capacity];

    if (pp_hash_match(ent, key, keylen))
      return ent;

    if (ent->key == (char *)PP_TOMBSTONE) {
      ent->key = key;
      ent->keylen = keylen;
      return ent;
    }

    if (ent->key == NULL) {
      ent->key = key;
      ent->keylen = keylen;
      map->used++;
      return ent;
    }
  }
  INNER_DIE("unreachable: pp_hash_get_or_insert");
}

static void *pp_hash_get2(PPHashMap *map, char *key, int keylen) {
  PPHashEntry *ent = pp_hash_get_entry(map, key, keylen);
  return ent ? ent->val : NULL;
}

static void pp_hash_put2(PPHashMap *map, char *key, int keylen, void *val) {
  PPHashEntry *ent = pp_hash_get_or_insert(map, key, keylen);
  ent->val = val;
}

static void pp_hash_delete2(PPHashMap *map, char *key, int keylen) {
  PPHashEntry *ent = pp_hash_get_entry(map, key, keylen);
  if (ent)
    ent->key = (char *)PP_TOMBSTONE;
}

typedef struct PPMacro PPMacro;
struct PPMacro {
  char *name;
  PPSrcLoc defined_at;
  PPToken *body; // replacement list tokens (no NEWLINE)
};

typedef struct {
  PPHashMap macros;
} PPContext;

static PPToken *pp_clone_range(PPToken *tok, PPToken *end) {
  PPToken head = {};
  PPToken *cur = &head;
  while (tok && tok != end) {
    PPToken *c = pp_clone_tok(tok);
    c->next = NULL;
    cur = cur->next = c;
    tok = tok->next;
  }
  return head.next;
}

static bool pp_is_identifier(PPToken *tok) {
  return tok && tok->kind == PPTOK_IDENTIFIER;
}

static void pp_list_append(PPToken **out_cur, PPToken *node) {
  (*out_cur)->next = node;
  *out_cur = node;
}

static void pp_list_append_list(PPToken **out_cur, PPToken *list) {
  while (list) {
    PPToken *next = list->next;
    list->next = NULL;
    pp_list_append(out_cur, list);
    list = next;
  }
}

static PPMacro *pp_macro_find(PPContext *ctx, PPToken *tok) {
  if (!ctx || !tok || tok->kind != PPTOK_IDENTIFIER)
    return NULL;
  return (PPMacro *)pp_hash_get2(&ctx->macros, (char *)tok->loc, tok->len);
}

static void pp_macro_undef(PPContext *ctx, char *name, int len) {
  if (!ctx)
    return;
  PPMacro *m = (PPMacro *)pp_hash_get2(&ctx->macros, name, len);
  if (!m)
    return;
  pp_hash_delete2(&ctx->macros, name, len);
  free_pptokens(m->body);
  free(m->name);
  free(m);
}

static void pp_macro_define_obj(PPContext *ctx, PPSrcLoc defined_at, char *name,
                               PPToken *body) {
  int len = (int)strlen(name);
  pp_macro_undef(ctx, name, len);

  PPMacro *m = calloc(1, sizeof(*m));
  if (!m)
    die_oom("allocating macro");
  m->name = name;
  m->defined_at = defined_at;
  m->body = body;
  pp_hash_put2(&ctx->macros, m->name, len, m);
}

static PPToken *pp_expand_list(PPContext *ctx, PPToken *tok);

static PPOrigin *pp_origin_new(const char *macro_name, PPSrcLoc expanded_at,
                               PPSrcLoc defined_at, PPOrigin *parent) {
  PPOrigin *o = calloc(1, sizeof(*o));
  if (!o)
    die_oom("allocating macro origin");
  o->macro_name = macro_name;
  o->expanded_at = expanded_at;
  o->defined_at = defined_at;
  o->parent = parent;
  return o;
}

static PPToken *pp_clone_tok_for_macro(const PPToken *body_tok,
                                       const PPToken *call_tok,
                                       const PPMacro *m) {
  PPToken *t = pp_clone_tok(body_tok);

  // origin: one new frame for this macro expansion, parented by caller origin.
  PPOrigin *parent = pp_origin_clone(call_tok->origin);
  t->origin = pp_origin_new(m->name, call_tok->spelling, m->defined_at, parent);

  // hideset: union(body, call) + macro name
  PPHideSet *hs = pp_hideset_union(body_tok->hideset, call_tok->hideset);
  hs = pp_hideset_add_name(hs, m->name);
  pp_hideset_free(t->hideset);
  t->hideset = hs;

  return t;
}

static PPToken *pp_expand_list(PPContext *ctx, PPToken *tok) {
  if (!ctx)
    return tok;

  PPToken head = {};
  PPToken *out_cur = &head;

  while (tok) {
    PPToken *next = tok->next;
    tok->next = NULL;

    if (tok->kind == PPTOK_IDENTIFIER) {
      PPMacro *m = pp_macro_find(ctx, tok);
      if (m && !pp_hideset_contains(tok->hideset, tok->loc, tok->len)) {
        // Replace identifier token with expanded macro body.
        for (PPToken *bp = m->body; bp; bp = bp->next) {
          PPToken *expanded = pp_clone_tok_for_macro(bp, tok, m);
          expanded->next = NULL;
          pp_list_append(&out_cur, expanded);
        }
        pp_free_tok(tok);
        tok = next;
        continue;
      }
    }

    pp_list_append(&out_cur, tok);
    tok = next;
  }

  // Expand recursively until no changes. Since we don't have function-like
  // macros yet, a few fixed iterations is enough; hideset prevents loops.
  bool changed;
  do {
    changed = false;
    PPToken out2 = {};
    PPToken *cur2 = &out2;

    PPToken *p = head.next;
    head.next = NULL;
    while (p) {
      PPToken *pn = p->next;
      p->next = NULL;
      if (p->kind == PPTOK_IDENTIFIER) {
        PPMacro *m = pp_macro_find(ctx, p);
        if (m && !pp_hideset_contains(p->hideset, p->loc, p->len)) {
          changed = true;
          for (PPToken *bp = m->body; bp; bp = bp->next) {
            PPToken *expanded = pp_clone_tok_for_macro(bp, p, m);
            expanded->next = NULL;
            pp_list_append(&cur2, expanded);
          }
          pp_free_tok(p);
          p = pn;
          continue;
        }
      }
      pp_list_append(&cur2, p);
      p = pn;
    }
    head.next = out2.next;
  } while (changed);

  return head.next;
}

typedef struct {
  PPContext *ctx;
  PPToken **out_cur;
  bool emit_text;
  bool stop_on_endif_like;
} PPGroupParser;

static PPToken *pp_parse_group(PPGroupParser *p, PPToken *tok);

static PPToken *pp_handle_text_line(PPGroupParser *p, PPToken *tok) {
  PPToken *line_end = tok;
  while (line_end && line_end->kind != PPTOK_EOF &&
         line_end->kind != PPTOK_NEWLINE)
    line_end = line_end->next;

  if (!p->emit_text)
    return pp_skip_to_line_end(tok);

  // Clone the line into an owned list, expand macros in it, and append to output.
  PPToken *line = pp_clone_range(tok, line_end);
  line = pp_expand_list(p->ctx, line);
  pp_list_append_list(p->out_cur, line);

  if (line_end && line_end->kind == PPTOK_NEWLINE) {
    pp_list_append(p->out_cur, pp_clone_tok(line_end));
    return line_end->next;
  }
  return line_end;
}

static PPToken *pp_handle_empty_directive(PPToken *tok) {
  // control-line: "#" new-line
  PPToken *trail = tok->next;
  if (trail && trail->kind != PPTOK_NEWLINE && trail->kind != PPTOK_EOF)
    pp_die_tok(trail, "extra token after #");
  return pp_skip_to_line_end(tok);
}

static PPToken *pp_handle_include(PPToken *tok) { return pp_skip_to_line_end(tok); }

static PPToken *pp_handle_include_next(PPToken *tok) {
  return pp_skip_to_line_end(tok);
}

static PPToken *pp_handle_define(PPContext *ctx, PPToken *tok) {
  // control-line:
  //   # define identifier replacement-list new-line
  // For now we only support object-like macros.
  PPToken *define_tok = tok->next;
  PPToken *name_tok = define_tok ? define_tok->next : NULL;
  if (!pp_directive_is(tok, "define") || !pp_is_identifier(name_tok))
    pp_die_tok(tok, "malformed #define");

  char *name = pp_strndup(name_tok->loc, name_tok->len);

  // Replacement-list: tokens up to NEWLINE.
  PPToken *line_end = name_tok->next;
  while (line_end && line_end->kind != PPTOK_EOF &&
         line_end->kind != PPTOK_NEWLINE)
    line_end = line_end->next;
  PPToken *body = pp_clone_range(name_tok->next, line_end);

  pp_macro_define_obj(ctx, name_tok->spelling, name, body);
  return pp_skip_to_line_end(tok);
}

static PPToken *pp_handle_undef(PPContext *ctx, PPToken *tok) {
  // control-line:
  //   # undef identifier new-line
  PPToken *undef_tok = tok->next;
  PPToken *name_tok = undef_tok ? undef_tok->next : NULL;
  if (!pp_directive_is(tok, "undef") || !pp_is_identifier(name_tok))
    pp_die_tok(tok, "malformed #undef");
  pp_macro_undef(ctx, (char *)name_tok->loc, name_tok->len);
  PPToken *trail = name_tok->next;
  if (trail && trail->kind != PPTOK_NEWLINE && trail->kind != PPTOK_EOF)
    pp_die_tok(trail, "extra token after #undef");
  return pp_skip_to_line_end(tok);
}

static PPToken *pp_handle_line(PPToken *tok) { return pp_skip_to_line_end(tok); }

static PPToken *pp_handle_error(PPToken *tok) { return pp_skip_to_line_end(tok); }

static PPToken *pp_handle_pragma(PPToken *tok) { return pp_skip_to_line_end(tok); }

static PPToken *pp_handle_non_directive(PPToken *tok) { return pp_skip_to_line_end(tok); }

static PPToken *pp_handle_control_line(PPGroupParser *p, PPToken *tok) {
  if (pp_is_empty_directive(tok))
    return pp_handle_empty_directive(tok);
  if (pp_directive_is(tok, "include"))
    return pp_handle_include(tok);
  if (pp_directive_is(tok, "include_next"))
    return pp_handle_include_next(tok);
  if (pp_directive_is(tok, "define"))
    return pp_handle_define(p->ctx, tok);
  if (pp_directive_is(tok, "undef"))
    return pp_handle_undef(p->ctx, tok);
  if (pp_directive_is(tok, "line"))
    return pp_handle_line(tok);
  if (pp_directive_is(tok, "error"))
    return pp_handle_error(tok);
  if (pp_directive_is(tok, "pragma"))
    return pp_handle_pragma(tok);

  // conditionals are handled elsewhere; unknown directives are non-directive.
  return pp_handle_non_directive(tok);
}

static PPToken *pp_handle_if_section(PPGroupParser *p, PPToken *tok) {
  // Parse:
  //   if-group (elif-group)* (else-group)? endif-line
  // We do not evaluate expressions, so we also do not emit any controlled text.
  PPSrcLoc started_at = tok->spelling;

  if (!(pp_directive_is(tok, "if") || pp_directive_is(tok, "ifdef") ||
        pp_directive_is(tok, "ifndef")))
    pp_die_tok(tok, "internal error: expected #if/#ifdef/#ifndef");

  // if-line
  tok = pp_skip_to_line_end(tok);
  // pp_handle_if_section is syntax-only for now, so we don't expand or emit
  // anything in controlled regions.
  PPGroupParser sub = *p;
  sub.emit_text = false;
  sub.stop_on_endif_like = true;
  tok = pp_parse_group(&sub, tok);

  // elif-groupsopt
  for (;;) {
    if (!tok || tok->kind == PPTOK_EOF)
      break;
    if (!pp_directive_is(tok, "elif"))
      break;
    tok = pp_skip_to_line_end(tok);
    tok = pp_parse_group(&sub, tok);
  }

  // else-groupopt
  if (tok && pp_directive_is(tok, "else")) {
    tok = pp_skip_to_line_end(tok);
    tok = pp_parse_group(&sub, tok);
  }

  // endif-line
  if (!tok || tok->kind == PPTOK_EOF) {
    DIE("%s:%d:%d: unterminated #if (missing #endif)", started_at.path,
        started_at.line_no, started_at.col_no);
  }

  if (!pp_directive_is(tok, "endif"))
    pp_die_tok(tok, "expected #endif");
  tok = pp_skip_to_line_end(tok);

  return tok;
}

static PPToken *pp_parse_group(PPGroupParser *p, PPToken *tok) {
  // Parse groupopt/group: a sequence of group-part.
  // When stop_on_endif_like is true, we stop before a line that begins with
  // #elif/#else/#endif (so the enclosing if-section parser can consume it).
  while (tok && tok->kind != PPTOK_EOF) {
    if (p->stop_on_endif_like && pp_is_directive_start(tok)) {
      if (pp_is_endif_like(tok))
        return tok;
    }

    if (!pp_is_directive_start(tok)) {
      tok = pp_handle_text_line(p, tok);
      continue;
    }

    if (pp_directive_is(tok, "if") || pp_directive_is(tok, "ifdef") ||
        pp_directive_is(tok, "ifndef")) {
      tok = pp_handle_if_section(p, tok);
      continue;
    }

    if (pp_is_endif_like(tok)) {
      // These should have been caught by stop_on_endif_like.
      pp_die_tok(tok, "stray conditional directive");
    }

    tok = pp_handle_control_line(p, tok);
  }
  return tok;
}

static PPToken *preprocess(PPToken *in) {
  PPToken head = {};
  PPToken *out_cur = &head;

  PPContext ctx = {};
  PPGroupParser p = {
      .ctx = &ctx,
      .out_cur = &out_cur,
      .emit_text = true,
      .stop_on_endif_like = false,
  };
  PPToken *tok = pp_parse_group(&p, in);

  if (!tok || tok->kind != PPTOK_EOF)
    pp_die_tok(tok, "internal error: expected EOF after preprocessing-file");

  out_cur->next = pp_clone_tok(tok);
  out_cur = out_cur->next;
  return head.next;
}

/* section: lexical analysis */

/* section: main function */
int main(int argc, char **argv) {
  parse_argv(argc, argv);
  if (opt.verbose)
    dump_options(stdout, &opt);

  if (opt.inputs.len == 0) {
    DIE_HINT("no input file");
  }
  validate_options(&opt);

  // Preprocessing-token tokenizer demo:
  //   - `-E`: print tokens (not a full preprocessor; just token stream)
  //   - `--tokens`: dump tokens to stderr
  if (opt.c_inputs.len == 0)
    DIE_HINT("no .c input files");

  if (opt.opt_E || opt.dump_tokens) {
    for (int i = 0; i < opt.c_inputs.len; i++) {
      const char *path = opt.c_inputs.data[i];
      PPFile f = pp_read_file(path);

      PPToken *pp = tokenlize(&f);
      PPToken *pp2 = NULL;

      if (opt.dump_tokens) {
        for (PPToken *tok = pp; tok; tok = tok->next) {
          pp_fprint_srcloc(stderr, tok->spelling);
          fprintf(stderr, ": %s%s%s", pp_tok_kind_name(tok->kind),
                  tok->at_bol ? "(BOL)" : "",
                  tok->kind == PPTOK_NEWLINE ? "" : ": ");
          if (tok->kind != PPTOK_NEWLINE)
            fwrite(tok->loc, 1, (size_t)tok->len, stderr);
          fputc('\n', stderr);
        }
      }

      if (opt.opt_E) {
        pp2 = preprocess(pp);
        for (PPToken *tok = pp2; tok; tok = tok->next) {
          if (tok->kind == PPTOK_NEWLINE) {
            fputc('\n', stdout);
          } else {
            if (tok->has_space)
              fputc(' ', stdout);
            fwrite(tok->loc, 1, (size_t)tok->len, stdout);
          }
        }
      }

      free_pptokens(pp2);
      free_pptokens(pp);
      pp_free_file(&f);
    }
  }

  return 0;
}
