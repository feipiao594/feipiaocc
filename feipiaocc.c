#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  TK_IDENT,
  TK_NUM,
  TK_PUNCT,
  TK_KEYWORD,
  TK_EOF,
} TokenKind;

typedef struct Token Token;
struct Token {
  TokenKind kind;
  Token *next;
  long val;
  char *loc;
  int len;
};

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

static char *current_input;
static char *current_filename;
static Token *token;

static void error_at(char *loc, char *fmt, ...) {
  va_list ap;
  int pos = loc - current_input;

  fprintf(stderr, "%s\n", current_input);
  fprintf(stderr, "%*s", pos, "");
  fprintf(stderr, "^ ");

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void error_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  int pos = tok->loc - current_input;

  fprintf(stderr, "%s\n", current_input);
  fprintf(stderr, "%*s", pos, "");
  fprintf(stderr, "^ ");

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

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

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = calloc(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  return tok;
}

static bool is_ident1(int c) {
  return isalpha(c) || c == '_';
}

static bool is_ident2(int c) {
  return isalnum(c) || c == '_';
}

static Token *tokenize(char *p) {
  Token head = {};
  Token *cur = &head;

  while (*p) {
    if (isspace(*p)) {
      p++;
      continue;
    }

    if (!strncmp(p, "return", 6) && !is_ident2(p[6])) {
      cur = cur->next = new_token(TK_KEYWORD, p, p + 6);
      p += 6;
      continue;
    }

    if (is_ident1(*p)) {
      char *start = p;
      p++;
      while (is_ident2(*p))
        p++;
      cur = cur->next = new_token(TK_IDENT, start, p);
      continue;
    }

    if (isdigit(*p)) {
      char *start = p;
      long val = strtol(p, &p, 10);
      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = val;
      continue;
    }

    if (*p == ';') {
      cur = cur->next = new_token(TK_PUNCT, p, p + 1);
      p++;
      continue;
    }

    error_at(p, "invalid token");
  }

  cur->next = new_token(TK_EOF, p, p);
  return head.next;
}

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
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: feipiaocc <file>\n");
    return 1;
  }

  current_filename = argv[1];
  current_input = read_file(current_filename);

  Token *tok = tokenize(current_input);
  Node *node = parse(tok);
  codegen(node);
  return 0;
}
