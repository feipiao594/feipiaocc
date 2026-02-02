#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* section: general tools*/
static void die_oom(const char *what) {
  fprintf(stderr, "error: out of memory while %s\n", what ? what : "allocating");
  exit(1);
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
  StrVec inputs;      // .c/.o/.a/.so/... (non-option inputs)
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
  strvec_push(&opt->inputs, values[0]);
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

static bool starts_with(const char *s, const char *prefix) {
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

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

#define ERRORF(...)                      \
  do {                                   \
    fprintf(stderr, "error: ");          \
    fprintf(stderr, __VA_ARGS__);        \
    fprintf(stderr, "\n");               \
  } while (0)

#define INNER_ERRORF(...)                      \
  do {                                   \
    fprintf(stderr, "inner error: ");          \
    fprintf(stderr, __VA_ARGS__);        \
    fprintf(stderr, "\n");               \
  } while (0)

#define DIE(...)                         \
  do {                                   \
    ERRORF(__VA_ARGS__);                 \
    exit(1);                             \
  } while (0)

#define INNER_DIE(...)                         \
  do {                                   \
    INNER_ERRORF(__VA_ARGS__);                 \
    exit(1);                             \
  } while (0)


#define DIE_HINT(...)                    \
  do {                                   \
    ERRORF(__VA_ARGS__);                 \
    print_errhint();                     \
    exit(1);                             \
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
}


/* section: preprocess part */


/* section: lexical analysis */


/* section: main function */
int main(int argc, char **argv) {
  parse_argv(argc, argv);
  if (opt.verbose) dump_options(stdout, &opt);

  if (opt.inputs.len == 0) {
    DIE_HINT("no input file");
  }
  return 0;
}
