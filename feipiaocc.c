#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* section: preprocess part */

/* section: lexical analysis */

/* section: parse arguments */
typedef struct {
  bool dump_tokens;
  bool dump_codegen;
  const char *input_path;
} Options;

typedef struct {
  const char *names[3];
  const char *help;
  int nargs;
  bool (*apply)(Options *opt, int nargs, const char **values);
} OptionSpec;

static Options opt = {
    .dump_tokens = false,
    .dump_codegen = true,
    .input_path = NULL,
};

static void print_help(FILE *out);
static void print_errhint();

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

static bool opt_set_input(Options *opt, int nargs, const char **values) {
  if (nargs != 1)
    return false;
  opt->input_path = values[0];
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
  {{(name1), NULL, NULL}, (helpstr), (nargs_), (applyfn)}

#define OPT2(name1, name2, helpstr, nargs_, applyfn)                           \
  {{(name1), (name2), NULL}, (helpstr), (nargs_), (applyfn)}

#define OPT3(name1, name2, name3, helpstr, nargs_, applyfn)                    \
  {{(name1), (name2), (name3)}, (helpstr), (nargs_), (applyfn)}

static const OptionSpec specs[] = {
    OPT2("-h", "--help", "show this help", 0, opt_help),
    OPT1("--tokens", "dump tokens then continue", 0, opt_set_dump_tokens),
    OPT1("--no-codegen", "parse only; do not emit code", 0, opt_set_no_codegen),
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

static bool match_option_name(const OptionSpec *spec, const char *arg) {
  for (size_t i = 0; i < sizeof(spec->names) / sizeof(*spec->names); i++) {
    const char *name = spec->names[i];
    if (!name)
      break;
    if (!strcmp(arg, name))
      return true;
  }
  return false;
}

static bool is_known_option(const char *arg) {
  if (!arg || arg[0] != '-' || arg[1] == '\0')
    return false;
  for (size_t i = 0; i < specs_len; i++)
    if (match_option_name(&specs[i], arg))
      return true;
  return false;
}

#define ERRORF(...)                      \
  do {                                   \
    fprintf(stderr, "error: ");          \
    fprintf(stderr, __VA_ARGS__);        \
    fprintf(stderr, "\n");               \
  } while (0)

#define DIE(...)                         \
  do {                                   \
    ERRORF(__VA_ARGS__);                 \
    exit(1);                             \
  } while (0)

#define DIE_HINT(...)                    \
  do {                                   \
    ERRORF(__VA_ARGS__);                 \
    print_errhint();                     \
    exit(1);                             \
  } while (0)

void parse_argv(int argc, char **argv) {
  bool stop_options = false;

  for (int i = 1; i < argc; i++) {
    char *arg = argv[i];

    if (!stop_options && !strcmp(arg, "--")) {
      stop_options = true;
      continue;
    }

    if (!stop_options && arg[0] == '-') {
      const OptionSpec *spec = NULL;
      for (size_t j = 0; j < specs_len; j++) {
        if (match_option_name(&specs[j], arg)) {
          spec = &specs[j];
          break;
        }
      }

      if (!spec) {
        DIE_HINT("unknown option: %s", arg);
      }

      const char **values = NULL;
      if (spec->nargs < 0)
        DIE("invalid nargs=%d for %s", spec->nargs, arg);

      if (spec->nargs != 0) {
        if (i + spec->nargs >= argc)
          DIE_HINT("option requires %d argument(s): %s", spec->nargs, arg);

        values = calloc((size_t)spec->nargs, sizeof(*values));
        for (int n = 0; n < spec->nargs; n++) {
          char *v = argv[i + 1];
          if (is_known_option(v))
            DIE_HINT("option requires %d argument(s): %s", spec->nargs, arg);
          values[n] = argv[++i];
        }
      }

      bool ok = spec->apply(&opt, spec->nargs, values);
      free(values);
      if (!ok)
        DIE("failed to apply option: %s", arg);
      continue;
    }

    if (opt.input_path) {
      DIE_HINT("multiple input files: %s", arg);
    }

    const char *v[] = {arg};
    if (!opt_set_input(&opt, 1, v))
      DIE("failed to set input file: %s", arg);
  }

  if (!opt.input_path) {
    DIE_HINT("no input file");
  }
}

int main(int argc, char **argv) {
  parse_argv(argc, argv);
  (void)opt;
  return 0;
}
