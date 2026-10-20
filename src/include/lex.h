#ifndef QUACKY_LEX_H
#define QUACKY_LEX_H

#include <stdbool.h>

/* Token kinds (02 section 2). T_RAW_LINE only ever follows T_TYPE. Key names
   like ctrl/enter/f5 are NOT here; they lex as T_IDENT and the parser resolves
   them in Phase 2 (01 section 1). */
typedef enum {
  T_EOF,
  T_NEWLINE,
  T_SEMI,
  T_INT,
  T_STRING,
  T_IDENT,
  T_RAW_LINE,
  /* keywords */
  T_IF,
  T_THEN,
  T_ELIF,
  T_ELSE,
  T_FI,
  T_WHILE,
  T_DO,
  T_DONE,
  T_FOR,
  T_IN,
  T_FUNC,
  T_END,
  T_RETURN,
  T_BREAK,
  T_CONTINUE,
  T_CONST,
  T_TRUE,
  T_FALSE,
  T_SLEEP,
  T_TYPE,
  T_TYPELN,
  T_TYPE_FILE,
  T_TYPELN_FILE,
  T_HOLD,
  T_RELEASE,
  T_DELAY,
  T_REPEAT,
  /* punctuation and operators */
  T_LPAREN,
  T_RPAREN,
  T_COMMA,
  T_ASSIGN,
  T_DOTDOT,
  T_PLUS,
  T_MINUS,
  T_STAR,
  T_SLASH,
  T_PERCENT,
  T_SHL,
  T_SHR,
  T_AMP,
  T_PIPE,
  T_CARET,
  T_TILDE,
  T_LT,
  T_LE,
  T_GT,
  T_GE,
  T_EQ,
  T_NE,
  T_BANG,
  T_ANDAND,
  T_OROR,
  T_ERROR
} TokKind;

/* A token is a slice into the source buffer, never a copy. For a string the
   slice covers the bytes between the quotes; escape decoding is the parser's
   job. line/col are 1-based and mark where the token starts. */
typedef struct {
  TokKind kind;
  const char *start;
  int len;
  int line, col;
} Token;

typedef struct {
  const char *filename;
  char *src;     /* caller-owned; normalised in place by lex_init */
  const char *p; /* current read position */
  int line, col;
  bool after_type; /* the one bit of raw-mode state (02 section 3.1) */
} Lexer;

void lex_init(Lexer *lx, const char *filename, char *src);
Token lex_next(Lexer *lx);
const char *tok_kind_name(TokKind k);

#endif
