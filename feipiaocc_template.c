#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Tokens ---
typedef enum {
  TK_IDENT,
  TK_NUM,
  TK_PUNCT,
  TK_KEYWORD,
  TK_STR,
  TK_EOF,
} TokenKind;

typedef struct Token Token;
struct Token {
  TokenKind kind;
  Token *next;
  long val;
  double fval;
  bool is_float;
  char *loc;
  int len;
  char *str;
  int str_len;
  bool is_wide;
};

// --- AST ---
typedef enum {
  ND_NUM,
  ND_RETURN,
} NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *lhs;
  long val;
};

// --- Global state ---
static char *current_input;
static char *current_filename;
static Token *token;

// --- Diagnostics ---
static void verror_at(char *loc, char *fmt, va_list ap) {
  char *line = current_input;
  int line_no = 1;

  for (char *p = current_input; p < loc; p++) {
    if (*p == '\n') {
      line_no++;
      line = p + 1;
    }
  }

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  int indent = loc - line;

  fprintf(stderr, "%s:%d: ", current_filename ? current_filename : "<input>",
          line_no);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  fprintf(stderr, "%.*s\n", (int)(end - line), line);
  fprintf(stderr, "%*s^\n", indent, "");
  exit(1);
}

static void error_at(char *loc, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(loc, fmt, ap);
  va_end(ap);
}

static void error_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->loc, fmt, ap);
  va_end(ap);
}

// --- Token helpers ---
static bool equal(Token *tok, char *op) {
  return tok->kind == TK_PUNCT && strlen(op) == (size_t)tok->len &&
         !strncmp(tok->loc, op, tok->len);
}

static Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_tok(tok, "expected '%s'", op);
  return tok->next;
}

static bool is_keyword(Token *tok, char *kw) {
  return tok->kind == TK_KEYWORD && strlen(kw) == (size_t)tok->len &&
         !strncmp(tok->loc, kw, tok->len);
}

static bool consume_keyword(Token **rest, Token *tok, char *kw) {
  if (!is_keyword(tok, kw))
    return false;
  *rest = tok->next;
  return true;
}

// --- Lexer ---
static bool is_keyword_str(char *p, int len) {
  static char *kw[] = {
      "return", "if",       "else",     "for",      "while",   "do",
      "switch", "case",     "default",  "break",    "continue","goto",
      "int",    "char",     "short",    "long",     "void",    "enum",
      "struct", "union",    "typedef",  "static",   "extern",  "sizeof",
      "const",  "volatile", "signed",   "unsigned", "float",   "double",
      "inline", "register", "auto",     "restrict", "_Bool",   "_Alignof",
      "_Alignas","_Atomic", "_Thread_local", "_Noreturn", "_Static_assert",
      "_Complex", "_Imaginary", "__attribute__", "asm"
  };

  for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++) {
    if ((int)strlen(kw[i]) == len && !strncmp(p, kw[i], len))
      return true;
  }
  return false;
}

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = calloc(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  return tok;
}

static int read_punct(char *p) {
  static char *kw[] = {
      "<<=", ">>=", "==", "!=", "<=", ">=", "->", "++", "--", "+=", "-=",
      "*=", "/=", "%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "...",
  };

  for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++) {
    int len = strlen(kw[i]);
    if (!strncmp(p, kw[i], len))
      return len;
  }

  return ispunct(*p) ? 1 : 0;
}

static bool read_int_suffix(char **new_pos, char *p);
static bool read_float_suffix(char **new_pos, char *p);

static Token *read_number(char **new_pos, char *p) {
  char *start = p;
  char *end = p;

  if (*p == '.' && isdigit(p[1])) {
    double fval = strtod(p, &end);
    read_float_suffix(&end, end);
    Token *tok = new_token(TK_NUM, start, end);
    tok->is_float = true;
    tok->fval = fval;
    *new_pos = end;
    return tok;
  }

  unsigned long val = strtoul(p, &end, 0);
  if (*end == '.' || *end == 'e' || *end == 'E' || *end == 'p' ||
      *end == 'P') {
    double fval = strtod(p, &end);
    read_float_suffix(&end, end);
    Token *tok = new_token(TK_NUM, start, end);
    tok->is_float = true;
    tok->fval = fval;
    *new_pos = end;
    return tok;
  }

  if (!read_int_suffix(&end, end) && strchr("uUlL", *end))
    error_at(end, "invalid integer suffix");

  Token *tok = new_token(TK_NUM, start, end);
  tok->val = val;
  tok->is_float = false;
  *new_pos = end;
  return tok;
}

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    int val = 0;
    int cnt = 0;
    while (cnt < 3 && '0' <= *p && *p <= '7') {
      val = (val << 3) + (*p - '0');
      p++;
      cnt++;
    }
    *new_pos = p;
    return val;
  }

  if (*p == 'x') {
    p++;
    int val = 0;
    while (isxdigit(*p)) {
      val = (val << 4) + (isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
      p++;
    }
    if (val == 0 && !isxdigit(p[-1]))
      error_at(p - 1, "invalid hex escape");
    *new_pos = p;
    return val;
  }

  if (*p == 'u' || *p == 'U') {
    int len = (*p == 'u') ? 4 : 8;
    p++;
    int val = 0;
    for (int i = 0; i < len; i++) {
      if (!isxdigit(*p))
        error_at(p, "invalid unicode escape");
      val = (val << 4) + (isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
      p++;
    }
    *new_pos = p;
    return val;
  }

  *new_pos = p + 1;
  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  case 'e': return 27;
  default: return *p;
  }
}

static void append_utf8(char *buf, int *len, int codepoint, char *loc) {
  if (codepoint < 0x80) {
    buf[(*len)++] = codepoint;
    return;
  }
  if (codepoint < 0x800) {
    buf[(*len)++] = 0xc0 | (codepoint >> 6);
    buf[(*len)++] = 0x80 | (codepoint & 0x3f);
    return;
  }
  if (codepoint < 0x10000) {
    buf[(*len)++] = 0xe0 | (codepoint >> 12);
    buf[(*len)++] = 0x80 | ((codepoint >> 6) & 0x3f);
    buf[(*len)++] = 0x80 | (codepoint & 0x3f);
    return;
  }
  if (codepoint <= 0x10ffff) {
    buf[(*len)++] = 0xf0 | (codepoint >> 18);
    buf[(*len)++] = 0x80 | ((codepoint >> 12) & 0x3f);
    buf[(*len)++] = 0x80 | ((codepoint >> 6) & 0x3f);
    buf[(*len)++] = 0x80 | (codepoint & 0x3f);
    return;
  }
  error_at(loc, "invalid unicode codepoint");
}

static bool is_ident1(int c) {
  return isalpha(c) || c == '_';
}

static bool is_ident2(int c) {
  return isalnum(c) || c == '_';
}

static void skip_ws_and_comments(char **pp) {
  char *p = *pp;
  for (;;) {
    if (isspace(*p)) {
      p++;
      continue;
    }

    if (!strncmp(p, "//", 2)) {
      p += 2;
      while (*p && *p != '\n')
        p++;
      continue;
    }

    if (!strncmp(p, "/*", 2)) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      p = q + 2;
      continue;
    }
    break;
  }
  *pp = p;
}

static bool read_int_suffix(char **new_pos, char *p) {
  char *s = p;
  bool seen_u = false;
  int l_cnt = 0;

  if (*p == 'u' || *p == 'U') {
    seen_u = true;
    p++;
  }

  if (*p == 'l' || *p == 'L') {
    l_cnt++;
    p++;
    if (*p == 'l' || *p == 'L') {
      l_cnt++;
      p++;
    }
  }

  if (!seen_u && (*p == 'u' || *p == 'U')) {
    seen_u = true;
    p++;
  }

  if (l_cnt > 2)
    return false;

  if (seen_u && (*p == 'u' || *p == 'U'))
    return false;

  if ((*p == 'l' || *p == 'L'))
    return false;

  *new_pos = p;
  return p != s;
}

static bool read_float_suffix(char **new_pos, char *p) {
  if (*p == 'f' || *p == 'F' || *p == 'l' || *p == 'L') {
    *new_pos = p + 1;
    return true;
  }
  *new_pos = p;
  return false;
}

static Token *tokenize(char *p) {
  Token head = {};
  Token *cur = &head;

  while (*p) {
    skip_ws_and_comments(&p);
    if (!*p)
      break;

    if (is_ident1(*p)) {
      char *start = p;
      p++;
      while (is_ident2(*p))
        p++;
      if (is_keyword_str(start, p - start))
        cur = cur->next = new_token(TK_KEYWORD, start, p);
      else
        cur = cur->next = new_token(TK_IDENT, start, p);
      continue;
    }

    if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
      cur = cur->next = read_number(&p, p);
      if (is_ident1(*p))
        error_at(p, "invalid number literal");
      continue;
    }

    if (*p == 'L' && p[1] == '\'') {
      char *start = p;
      p += 2;
      if (*p == '\0')
        error_at(start, "unclosed char literal");

      int c;
      if (*p == '\\')
        c = read_escaped_char(&p, p + 1);
      else
        c = (unsigned char)*p++;

      if (*p != '\'')
        error_at(start, "char literal too long");
      p++;

      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = c;
      cur->is_wide = true;
      continue;
    }

    if (*p == '\'') {
      char *start = p++;
      if (*p == '\0')
        error_at(start, "unclosed char literal");

      int c;
      if (*p == '\\')
        c = read_escaped_char(&p, p + 1);
      else
        c = (unsigned char)*p++;

      if (c > 0xff)
        error_at(start, "character literal out of range");
      if (*p != '\'')
        error_at(start, "char literal too long");
      p++;

      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = c;
      continue;
    }

    if (*p == '"' || (*p == 'L' && p[1] == '"')) {
      char *start = p;
      bool is_wide = false;
      char *buf = calloc(1, strlen(p) + 1);
      int len = 0;

      for (;;) {
        if (*p == 'L') {
          is_wide = true;
          p++;
        }
        if (*p != '"')
          error_at(p, "expected string literal");
        p++;

        while (*p && *p != '"') {
          if (*p == '\\') {
            char *esc = p;
            int c = read_escaped_char(&p, p + 1);
            append_utf8(buf, &len, c, esc);
          } else {
            buf[len++] = *p++;
          }
        }

        if (*p != '"')
          error_at(start, "unclosed string literal");
        p++;

        char *q = p;
        skip_ws_and_comments(&q);
        if (*q == '"' || (*q == 'L' && q[1] == '"')) {
          p = q;
          continue;
        }
        break;
      }

      cur = cur->next = new_token(TK_STR, start, p);
      cur->str = buf;
      cur->str_len = len;
      cur->is_wide = is_wide;
      continue;
    }

    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
      p += punct_len;
      continue;
    }

    error_at(p, "invalid token");
  }

  cur->next = new_token(TK_EOF, p, p);
  return head.next;
}

// --- Token printer ---
static void print_token_kind(TokenKind kind) {
  switch (kind) {
  case TK_IDENT: printf("IDENT"); return;
  case TK_NUM: printf("NUM"); return;
  case TK_PUNCT: printf("PUNCT"); return;
  case TK_KEYWORD: printf("KW"); return;
  case TK_STR: printf("STR"); return;
  case TK_EOF: printf("EOF"); return;
  }
}

static void dump_tokens(Token *tok) {
  for (Token *t = tok; t; t = t->next) {
    print_token_kind(t->kind);
    printf(" ");
    if (t->kind == TK_NUM) {
      if (t->is_float)
        printf("%g", t->fval);
      else
        printf("%ld", t->val);
    } else if (t->kind == TK_STR) {
      if (t->is_wide)
        printf("L");
      printf("\"");
      for (int i = 0; i < t->str_len; i++) {
        char c = t->str[i];
        if (c == '\n')
          printf("\\n");
        else if (c == '\t')
          printf("\\t");
        else if (c == '"')
          printf("\\\"");
        else if (c == '\\')
          printf("\\\\");
        else
          putchar(c);
      }
      printf("\"");
    } else {
      printf("%.*s", t->len, t->loc);
    }
    printf("\n");

    if (t->kind == TK_EOF)
      break;
  }
}

// --- Source normalization ---
static int trigraph_char(int c) {
  switch (c) {
  case '=': return '#';
  case '/': return '\\';
  case '\'': return '^';
  case '(': return '[';
  case ')': return ']';
  case '!': return '|';
  case '<': return '{';
  case '>': return '}';
  case '-': return '~';
  }
  return 0;
}

static char *normalize_source(char *p) {
  size_t cap = strlen(p) * 2 + 1;
  char *buf = calloc(1, cap);
  size_t i = 0;

  while (*p) {
    if (p[0] == '?' && p[1] == '?' && p[2]) {
      int tc = trigraph_char(p[2]);
      if (tc) {
        buf[i++] = tc;
        p += 3;
        continue;
      }
    }

    if (p[0] == '%' && p[1] == ':' && p[2] == '%' && p[3] == ':') {
      buf[i++] = '#';
      buf[i++] = '#';
      p += 4;
      continue;
    }

    if (p[0] == '<' && p[1] == ':') {
      buf[i++] = '[';
      p += 2;
      continue;
    }
    if (p[0] == ':' && p[1] == '>') {
      buf[i++] = ']';
      p += 2;
      continue;
    }
    if (p[0] == '<' && p[1] == '%') {
      buf[i++] = '{';
      p += 2;
      continue;
    }
    if (p[0] == '%' && p[1] == '>') {
      buf[i++] = '}';
      p += 2;
      continue;
    }
    if (p[0] == '%' && p[1] == ':') {
      buf[i++] = '#';
      p += 2;
      continue;
    }

    buf[i++] = *p++;
  }

  buf[i] = '\0';
  return buf;
}

// --- Parser ---
static Node *new_node(NodeKind kind) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  return node;
}

static Node *new_num(long val) {
  Node *node = new_node(ND_NUM);
  node->val = val;
  return node;
}

static Node *expr(void) {
  if (token->kind != TK_NUM)
    error_tok(token, "expected a number");
  Node *node = new_num(token->val);
  token = token->next;
  return node;
}

static Node *stmt(void) {
  if (consume_keyword(&token, token, "return")) {
    Node *node = new_node(ND_RETURN);
    node->lhs = expr();
    token = skip(token, ";");
    return node;
  }

  error_tok(token, "expected 'return'");
  return NULL;
}

static Node *parse(Token *tok) {
  token = tok;
  Node *node = stmt();
  if (token->kind != TK_EOF)
    error_tok(token, "extra tokens");
  return node;
}

// --- Codegen ---
static void gen_expr(Node *node) {
  if (node->kind != ND_NUM)
    return;
  printf("  mov $%ld, %%rax\n", node->val);
}

static void gen_stmt(Node *node) {
  if (node->kind != ND_RETURN)
    return;
  gen_expr(node->lhs);
  printf("  jmp .L.return\n");
}

static void codegen(Node *node) {
  printf("  .globl main\n");
  printf("  .text\n");
  printf("main:\n");
  printf("  push %%rbp\n");
  printf("  mov %%rsp, %%rbp\n");
  gen_stmt(node);
  printf(".L.return:\n");
  printf("  mov %%rbp, %%rsp\n");
  printf("  pop %%rbp\n");
  printf("  ret\n");
}

// --- File IO ---
static char *read_file(char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }

  fseek(fp, 0, SEEK_END);
  size_t size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *buf = calloc(1, size + 2);
  fread(buf, 1, size, fp);
  fclose(fp);

  if (size == 0 || buf[size - 1] != '\n')
    buf[size++] = '\n';
  buf[size] = '\0';
  return normalize_source(buf);
}

// --- Driver ---
int main(int argc, char **argv) {
  bool dump_tokens_flag = false;
  bool dump_codegen_flag = true;

  if (argc < 2) {
    fprintf(stderr, "usage: feipiaocc <file> [--tokens] [--no-codegen]\n");
    return 1;
  }

  current_filename = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--tokens")) {
      dump_tokens_flag = true;
      continue;
    }
    if (!strcmp(argv[i], "--no-codegen")) {
      dump_codegen_flag = false;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    }
    if (current_filename) {
      fprintf(stderr, "multiple input files: %s\n", argv[i]);
      return 1;
    }
    current_filename = argv[i];
  }

  if (!current_filename) {
    fprintf(stderr, "no input file\n");
    return 1;
  }

  current_input = read_file(current_filename);

  Token *tok = tokenize(current_input);
  if (dump_tokens_flag) {
    dump_tokens(tok);
    if (!dump_codegen_flag)
      return 0;
  }
  Node *node = parse(tok);
  if (dump_codegen_flag)
    codegen(node);
  return 0;
}
