/* The lexer: one pass over a NUL-terminated source buffer, emitting tokens that
   are slices into it. Structured like a textbook hand-written scanner with one
   character of lookahead. The one unusual part is raw mode after `type`
   (02 section 3). */

#include "lex.h"
#include "parse.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_cont(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

/* Consume one byte, keeping line/col current. Never called at the NUL. */
static char advance(Lexer *lx) {
  char c = *lx->p++;
  if (c == '\n') {
    lx->line++;
    lx->col = 1;
  } else {
    lx->col++;
  }
  return c;
}

static Token mk(Lexer *lx, TokKind kind, const char *start, int line, int col) {
  Token t;
  t.kind = kind;
  t.start = start;
  t.len = (int)(lx->p - start);
  t.line = line;
  t.col = col;
  return t;
}

static TokKind keyword_kind(const char *s, int len) {
  static const struct {
    const char *kw;
    TokKind kind;
  } kws[] = {
      {"if", T_IF},
      {"then", T_THEN},
      {"elif", T_ELIF},
      {"else", T_ELSE},
      {"fi", T_FI},
      {"while", T_WHILE},
      {"do", T_DO},
      {"done", T_DONE},
      {"for", T_FOR},
      {"in", T_IN},
      {"func", T_FUNC},
      {"end", T_END},
      {"return", T_RETURN},
      {"break", T_BREAK},
      {"continue", T_CONTINUE},
      {"const", T_CONST},
      {"true", T_TRUE},
      {"false", T_FALSE},
      {"sleep", T_SLEEP},
      {"type", T_TYPE},
      {"typeln", T_TYPELN},
      {"type_file", T_TYPE_FILE},
      {"typeln_file", T_TYPELN_FILE},
      {"import", T_IMPORT},
      {"hold", T_HOLD},
      {"release", T_RELEASE},
      {"delay", T_DELAY},
      {"keydelay", T_KEYDELAY},
      {"fastmode", T_FASTMODE},
      {"ultramode", T_ULTRAMODE},
      {"repeat", T_REPEAT},
  };
  for (size_t i = 0; i < sizeof kws / sizeof kws[0]; i++) {
    if (strlen(kws[i].kw) == (size_t)len &&
        memcmp(s, kws[i].kw, (size_t)len) == 0)
      return kws[i].kind;
  }
  return T_IDENT;
}

/* Spaces, tabs and comments. Newlines and semicolons are tokens, so this stops
   in front of them. A line comment deliberately leaves its terminating newline
   for the separator logic (01 section 1). */
static void skip_trivia(Lexer *lx) {
  for (;;) {
    char c = *lx->p;
    if (c == ' ' || c == '\t') {
      advance(lx);
      continue;
    }
    if (c == '#') {
      if (lx->p[1] == '[') {
        int line = lx->line, col = lx->col; /* mark the opening #[ */
        advance(lx);
        advance(lx);
        while (*lx->p && !(*lx->p == ']' && lx->p[1] == '#'))
          advance(lx);
        if (*lx->p == '\0')
          syntax_error(lx->filename, lx->src, line, col,
                       "unclosed block comment");
        advance(lx);
        advance(lx); /* consume ]# */
        continue;
      }
      while (*lx->p && *lx->p != '\n')
        advance(lx);
      continue;
    }
    break;
  }
}

/* After `type` and exactly one space, the rest of the line is one raw slice;
   `#`, `"` and `;` carry no meaning here. The newline is left for the next
   call (02 section 3.1). */
static Token raw_line(Lexer *lx) {
  lx->after_type = false;
  if (*lx->p == ' ')
    advance(lx);
  int line = lx->line, col = lx->col;
  const char *start = lx->p;
  while (*lx->p && *lx->p != '\n')
    advance(lx);
  Token t;
  t.kind = T_RAW_LINE;
  t.start = start;
  t.len = (int)(lx->p - start);
  t.line = line;
  t.col = col;
  return t;
}

static Token number(Lexer *lx, const char *start, int line, int col) {
  int base = 10;
  const char *digits = lx->p;
  if (lx->p[0] == '0' && lx->p[1] == 'x') {
    base = 16;
    advance(lx);
    advance(lx);
    digits = lx->p;
    while (is_hex(*lx->p))
      advance(lx);
  } else if (lx->p[0] == '0' && lx->p[1] == 'b') {
    base = 2;
    advance(lx);
    advance(lx);
    digits = lx->p;
    while (*lx->p == '0' || *lx->p == '1')
      advance(lx);
  } else {
    while (*lx->p >= '0' && *lx->p <= '9')
      advance(lx);
  }

  if (lx->p == digits)
    syntax_error(lx->filename, lx->src, line, col,
                 "integer literal has no digits");

  /* Range-check through strtoull so an overflow is a clean syntax error, never
     undefined behaviour (02 section 3). The value itself is re-parsed by the
     parser; here we only validate. */
  errno = 0;
  char *endp;
  unsigned long long v = strtoull(digits, &endp, base);
  if (errno == ERANGE || v > (unsigned long long)INT64_MAX)
    syntax_error(lx->filename, lx->src, line, col,
                 "integer literal out of range");

  return mk(lx, T_INT, start, line, col);
}

/* The slice is the content between the quotes; escapes stay encoded for the
   parser. A newline or EOF before the closing quote is an unterminated string
   reported at the opening quote. */
static Token string(Lexer *lx, int line, int col) {
  advance(lx); /* opening quote */
  const char *inner = lx->p;
  for (;;) {
    char c = *lx->p;
    if (c == '"')
      break;
    if (c == '\0' || c == '\n')
      syntax_error(lx->filename, lx->src, line, col, "unterminated string");
    if (c == '\\') {
      advance(lx); /* backslash */
      if (*lx->p == '\0' || *lx->p == '\n')
        syntax_error(lx->filename, lx->src, line, col, "unterminated string");
      advance(lx); /* the escaped byte, so \" never ends it */
      continue;
    }
    advance(lx);
  }
  Token t;
  t.kind = T_STRING;
  t.start = inner;
  t.len = (int)(lx->p - inner);
  t.line = line;
  t.col = col;
  advance(lx); /* closing quote */
  return t;
}

void lex_init(Lexer *lx, const char *filename, char *src) {
  /* Drop the \r of every CRLF in place; a lone \r is kept (01 section 1). */
  char *w = src;
  for (char *r = src; *r; r++) {
    if (r[0] == '\r' && r[1] == '\n')
      continue;
    *w++ = *r;
  }
  *w = '\0';

  lx->filename = filename;
  lx->src = src;
  lx->p = src;
  lx->line = 1;
  lx->col = 1;
  lx->after_type = false;
}

Token lex_next(Lexer *lx) {
  if (lx->after_type)
    return raw_line(lx);

  skip_trivia(lx);

  int line = lx->line, col = lx->col;
  const char *start = lx->p;
  char c = *lx->p;

  if (c == '\0')
    return mk(lx, T_EOF, start, line, col);

  /* A maximal run of newlines and semicolons (and the trivia between them)
     collapses into one separator token, which keeps the parser simple
     (02 section 3). The kind reflects whether a newline was in the run. */
  if (c == '\n' || c == ';') {
    bool sawnl = false;
    for (;;) {
      if (*lx->p == '\n') {
        sawnl = true;
        advance(lx);
      } else if (*lx->p == ';') {
        advance(lx);
      } else
        break;
      skip_trivia(lx);
    }
    return mk(lx, sawnl ? T_NEWLINE : T_SEMI, start, line, col);
  }

  if (is_ident_start(c)) {
    while (is_ident_cont(*lx->p))
      advance(lx);
    Token t =
        mk(lx, keyword_kind(start, (int)(lx->p - start)), start, line, col);
    if (t.kind == T_TYPE || t.kind == T_TYPELN || t.kind == T_IMPORT)
      lx->after_type = true; /* import takes an unquoted rest-of-line path */
    return t;
  }

  if (c >= '0' && c <= '9')
    return number(lx, start, line, col);
  if (c == '"')
    return string(lx, line, col);

  advance(lx);
  switch (c) {
  case '(':
    return mk(lx, T_LPAREN, start, line, col);
  case ')':
    return mk(lx, T_RPAREN, start, line, col);
  case ',':
    return mk(lx, T_COMMA, start, line, col);
  case '+':
    return mk(lx, T_PLUS, start, line, col);
  case '-':
    return mk(lx, T_MINUS, start, line, col);
  case '*':
    return mk(lx, T_STAR, start, line, col);
  case '/':
    return mk(lx, T_SLASH, start, line, col);
  case '%':
    return mk(lx, T_PERCENT, start, line, col);
  case '^':
    return mk(lx, T_CARET, start, line, col);
  case '~':
    return mk(lx, T_TILDE, start, line, col);
  case '=':
    if (*lx->p == '=') {
      advance(lx);
      return mk(lx, T_EQ, start, line, col);
    }
    return mk(lx, T_ASSIGN, start, line, col);
  case '!':
    if (*lx->p == '=') {
      advance(lx);
      return mk(lx, T_NE, start, line, col);
    }
    return mk(lx, T_BANG, start, line, col);
  case '<':
    if (*lx->p == '=') {
      advance(lx);
      return mk(lx, T_LE, start, line, col);
    }
    if (*lx->p == '<') {
      advance(lx);
      return mk(lx, T_SHL, start, line, col);
    }
    return mk(lx, T_LT, start, line, col);
  case '>':
    if (*lx->p == '=') {
      advance(lx);
      return mk(lx, T_GE, start, line, col);
    }
    if (*lx->p == '>') {
      advance(lx);
      return mk(lx, T_SHR, start, line, col);
    }
    return mk(lx, T_GT, start, line, col);
  case '&':
    if (*lx->p == '&') {
      advance(lx);
      return mk(lx, T_ANDAND, start, line, col);
    }
    return mk(lx, T_AMP, start, line, col);
  case '|':
    if (*lx->p == '|') {
      advance(lx);
      return mk(lx, T_OROR, start, line, col);
    }
    return mk(lx, T_PIPE, start, line, col);
  case '.':
    if (*lx->p == '.') {
      advance(lx);
      return mk(lx, T_DOTDOT, start, line, col);
    }
    syntax_error(lx->filename, lx->src, line, col, "unexpected character '.'");
  }
  syntax_error(lx->filename, lx->src, line, col, "unexpected character '%c'",
               c);
}

const char *tok_kind_name(TokKind k) {
  switch (k) {
  case T_EOF:
    return "T_EOF";
  case T_NEWLINE:
    return "T_NEWLINE";
  case T_SEMI:
    return "T_SEMI";
  case T_INT:
    return "T_INT";
  case T_STRING:
    return "T_STRING";
  case T_IDENT:
    return "T_IDENT";
  case T_RAW_LINE:
    return "T_RAW_LINE";
  case T_IF:
    return "T_IF";
  case T_THEN:
    return "T_THEN";
  case T_ELIF:
    return "T_ELIF";
  case T_ELSE:
    return "T_ELSE";
  case T_FI:
    return "T_FI";
  case T_WHILE:
    return "T_WHILE";
  case T_DO:
    return "T_DO";
  case T_DONE:
    return "T_DONE";
  case T_FOR:
    return "T_FOR";
  case T_IN:
    return "T_IN";
  case T_FUNC:
    return "T_FUNC";
  case T_END:
    return "T_END";
  case T_RETURN:
    return "T_RETURN";
  case T_BREAK:
    return "T_BREAK";
  case T_CONTINUE:
    return "T_CONTINUE";
  case T_CONST:
    return "T_CONST";
  case T_TRUE:
    return "T_TRUE";
  case T_FALSE:
    return "T_FALSE";
  case T_SLEEP:
    return "T_SLEEP";
  case T_TYPE:
    return "T_TYPE";
  case T_TYPELN:
    return "T_TYPELN";
  case T_TYPE_FILE:
    return "T_TYPE_FILE";
  case T_TYPELN_FILE:
    return "T_TYPELN_FILE";
  case T_IMPORT:
    return "T_IMPORT";
  case T_HOLD:
    return "T_HOLD";
  case T_RELEASE:
    return "T_RELEASE";
  case T_DELAY:
    return "T_DELAY";
  case T_KEYDELAY:
    return "T_KEYDELAY";
  case T_FASTMODE:
    return "T_FASTMODE";
  case T_ULTRAMODE:
    return "T_ULTRAMODE";
  case T_REPEAT:
    return "T_REPEAT";
  case T_LPAREN:
    return "T_LPAREN";
  case T_RPAREN:
    return "T_RPAREN";
  case T_COMMA:
    return "T_COMMA";
  case T_ASSIGN:
    return "T_ASSIGN";
  case T_DOTDOT:
    return "T_DOTDOT";
  case T_PLUS:
    return "T_PLUS";
  case T_MINUS:
    return "T_MINUS";
  case T_STAR:
    return "T_STAR";
  case T_SLASH:
    return "T_SLASH";
  case T_PERCENT:
    return "T_PERCENT";
  case T_SHL:
    return "T_SHL";
  case T_SHR:
    return "T_SHR";
  case T_AMP:
    return "T_AMP";
  case T_PIPE:
    return "T_PIPE";
  case T_CARET:
    return "T_CARET";
  case T_TILDE:
    return "T_TILDE";
  case T_LT:
    return "T_LT";
  case T_LE:
    return "T_LE";
  case T_GT:
    return "T_GT";
  case T_GE:
    return "T_GE";
  case T_EQ:
    return "T_EQ";
  case T_NE:
    return "T_NE";
  case T_BANG:
    return "T_BANG";
  case T_ANDAND:
    return "T_ANDAND";
  case T_OROR:
    return "T_OROR";
  case T_ERROR:
    return "T_ERROR";
  }
  return "T_?";
}
