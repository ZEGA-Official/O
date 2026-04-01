// ============================================================
//  O Language Compiler — o_lexer.h
//  Lexer / tokenizer for the O language
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"

typedef enum {
    TOK_INT_LIT, TOK_FLOAT_LIT, TOK_STRING_LIT, TOK_CHAR_LIT, TOK_IDENT,
    TOK_KW_FN, TOK_KW_LET, TOK_KW_CONST, TOK_KW_RETURN,
    TOK_KW_IF, TOK_KW_ELSE, TOK_KW_WHILE, TOK_KW_FOR,
    TOK_KW_BREAK, TOK_KW_CONTINUE, TOK_KW_STRUCT, TOK_KW_UNION,
    TOK_KW_ENUM, TOK_KW_IMPORT, TOK_KW_EXTERN, TOK_KW_DEFER,
    TOK_KW_AS, TOK_KW_SIZEOF, TOK_KW_ALIGNOF,
    TOK_KW_NULL, TOK_KW_TRUE, TOK_KW_FALSE,
    TOK_KW_INLINE, TOK_KW_VOLATILE, TOK_KW_MUT,
    TOK_KW_VOID, TOK_KW_BOOL,
    TOK_KW_I8, TOK_KW_I16, TOK_KW_I32, TOK_KW_I64,
    TOK_KW_U8, TOK_KW_U16, TOK_KW_U32, TOK_KW_U64,
    TOK_KW_F32, TOK_KW_F64, TOK_KW_ISIZE, TOK_KW_USIZE,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COLON, TOK_COMMA, TOK_DOT,
    TOK_DOTDOT, TOK_DOTDOTDOT, TOK_ARROW,
    TOK_PLUS_PLUS, TOK_MINUS_MINUS, TOK_FAT_ARROW,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG,
    TOK_LSHIFT, TOK_RSHIFT, TOK_AMP_AMP, TOK_PIPE_PIPE,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN,
    TOK_AMP_ASSIGN, TOK_PIPE_ASSIGN, TOK_CARET_ASSIGN,
    TOK_LSHIFT_ASSIGN, TOK_RSHIFT_ASSIGN,
    TOK_AT, TOK_HASH, TOK_QUESTION,
    TOK_EOF, TOK_ERROR,
    TOK_COUNT
} TokKind;

typedef struct {
    TokKind  kind;
    StrView  text;
    SrcLoc   loc;
    union {
        u64  int_val;
        f64  flt_val;
    };
} Token;

typedef struct {
    const char *src;
    usize       src_len;
    usize       pos;
    u32         line;
    u32         col;
    const char *filename;
    Arena      *arena;
    u32         error_count;
} Lexer;

void  lexer_init(Lexer *l, Arena *arena,
                 const char *filename,
                 const char *src, usize src_len);
Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
const char *tok_kind_str(TokKind k);
bool        tok_is_assign_op(TokKind k);
bool        tok_is_cmp_op(TokKind k);
bool        tok_is_type_keyword(TokKind k);
