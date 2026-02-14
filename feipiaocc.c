#include <ctype.h>
#include <stdbool.h>
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

static bool opt_add_ld_arg_literal(Options *opt, int nargs, const char **values) {
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
  OPTP1("-l", "link with library (pass through to linker)", 1, opt_add_ld_arg_literal),
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
// - Spaces/tabs/etc (excluding '\n') are skipped and recorded via `has_space=true`
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
  char *contents; // NUL-terminated, normalized to '\n'
} PPFile;

typedef struct PPToken PPToken;
struct PPToken {
  PPTokenKind kind;
  const char *loc;
  int len;
  int line_no;
  bool at_bol;
  bool has_space;
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
  bool at_bol;
  bool has_space;
  PPCommentMode comment_mode;
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

  PPFile f = {.path = path, .contents = buf};
  return f;
}

static void pp_free_file(PPFile *file) {
  free(file->contents);
}

static void pp_tokenizer_init(PPTokenizer *tz, PPFile *file) {
  *tz = (PPTokenizer){
      .file = file,
      .cur = file->contents,
      .line_no = 1,
      .at_bol = true,
      .has_space = false,
      .comment_mode = PP_COMMENT_NONE,
  };
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
                           const char *end, int line_no, bool at_bol,
                           bool has_space) {
  (void)tz;
  return (PPToken){
      .kind = kind,
      .loc = start,
      .len = (int)(end - start),
      .line_no = line_no,
      .at_bol = at_bol,
      .has_space = has_space,
      .next = NULL,
  };
}

static bool pp_try_quoted_literal_end(PPTokenizer *tz, const char *p, char quote,
                                      const char **end_out) {
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
  *out =
      pp_make_tok(tz, kind, start, end, tz->line_no, tok_at_bol, tok_has_space);
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
  *out = pp_make_tok(tz, PPTOK_PP_NUMBER, start, end, tz->line_no, tok_at_bol,
                     tok_has_space);
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
  *out = pp_make_tok(tz, PPTOK_IDENTIFIER, start, end, tz->line_no, tok_at_bol,
                     tok_has_space);
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
  *out = pp_make_tok(tz, PPTOK_PUNCTUATOR, p, end, tz->line_no, tok_at_bol,
                     tok_has_space);
  tz->cur = end;
  tz->at_bol = false;
  tz->has_space = false;
  return true;
}

static bool pp_try_newline(PPTokenizer *tz, const char *p, PPToken *out) {
  if (*p != '\n')
    return false;

  *out = pp_make_tok(tz, PPTOK_NEWLINE, p, p + 1, tz->line_no, tz->at_bol,
                     tz->has_space);
  tz->cur = p + 1;
  tz->line_no++;
  tz->at_bol = true;
  tz->has_space = false;
  return true;
}

static bool pp_try_eof(PPTokenizer *tz, const char *p, PPToken *out) {
  if (*p != '\0')
    return false;

  *out =
      pp_make_tok(tz, PPTOK_EOF, p, p, tz->line_no, tz->at_bol, tz->has_space);
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
  //   our preprocessor implementation (directives are line-based; see C11 6.10).
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
      *out = pp_make_tok(tz, PPTOK_NEWLINE, p, p + 1, tz->line_no, tz->at_bol,
                         tz->has_space);
      tz->cur = p + 1;
      tz->line_no++;
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
    *out = pp_make_tok(tz, PPTOK_OTHER, p, p + 1, tz->line_no, tok_at_bol,
                       tok_has_space);
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
    free(tok);
    tok = next;
  }
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

      for (PPToken *tok = pp; tok; tok = tok->next) {
        if (opt.dump_tokens) {
          fprintf(stderr, "%s:%d: %s%s%s", path, tok->line_no,
                  pp_tok_kind_name(tok->kind), tok->at_bol ? "(BOL)" : "",
                  tok->kind == PPTOK_NEWLINE ? "" : ": ");
          if (tok->kind != PPTOK_NEWLINE)
            fwrite(tok->loc, 1, (size_t)tok->len, stderr);
          fputc('\n', stderr);
        }
      }

      free_pptokens(pp);
      pp_free_file(&f);
    }
  }

  return 0;
}
