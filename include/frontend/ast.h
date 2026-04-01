// ============================================================
//  O Language Compiler — o_ast.h
//  Abstract Syntax Tree node definitions
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "frontend/lexer.h"

typedef struct TypeNode   TypeNode;
typedef struct Expr       Expr;
typedef struct Stmt       Stmt;
typedef struct Decl       Decl;
typedef struct AstModule  AstModule;

typedef enum {
    TY_VOID, TY_BOOL,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64, TY_ISIZE, TY_USIZE,
    TY_POINTER, TY_ARRAY, TY_SLICE, TY_FUNC,
    TY_STRUCT, TY_UNION, TY_ENUM, TY_NAMED,
} TypeKind;

struct TypeNode {
    TypeKind kind;
    SrcLoc   loc;
    union {
        struct { TypeNode *pointee; bool is_mut; }                  ptr;
        struct { TypeNode *elem;    u64 count; }                    arr;
        struct { TypeNode *elem; }                                  slice;
        struct { TypeNode **params; TypeNode *ret;
                 u32 param_count; bool variadic; }                  func;
        StrView name;
    };
};

static inline bool ty_is_integer(TypeKind k) { return k >= TY_I8 && k <= TY_USIZE; }
static inline bool ty_is_signed(TypeKind k)  { return (k >= TY_I8 && k <= TY_I64) || k == TY_ISIZE; }
static inline bool ty_is_float(TypeKind k)   { return k == TY_F32 || k == TY_F64; }
static inline bool ty_is_numeric(TypeKind k) { return ty_is_integer(k) || ty_is_float(k); }

static inline u32 ty_primitive_size(TypeKind k) {
    switch (k) {
        case TY_BOOL: case TY_I8:  case TY_U8:  return 1;
        case TY_I16:  case TY_U16:              return 2;
        case TY_I32:  case TY_U32: case TY_F32: return 4;
        case TY_I64:  case TY_U64: case TY_F64: return 8;
        case TY_ISIZE: case TY_USIZE: case TY_POINTER: return 8;
        default: return 0;
    }
}

typedef enum {
    EXPR_INT_LIT, EXPR_FLOAT_LIT, EXPR_STRING_LIT,
    EXPR_CHAR_LIT, EXPR_BOOL_LIT, EXPR_NULL_LIT,
    EXPR_IDENT, EXPR_CALL, EXPR_INDEX, EXPR_FIELD,
    EXPR_DEREF, EXPR_ADDR_OF, EXPR_SIZEOF, EXPR_ALIGNOF, EXPR_CAST,
    EXPR_UNARY, EXPR_BINARY, EXPR_TERNARY,
    EXPR_STRUCT_INIT, EXPR_ARRAY_INIT,
    EXPR_COUNT
} ExprKind;

typedef enum {
    UNOP_NEG, UNOP_NOT, UNOP_BNOT,
    UNOP_PRE_INC, UNOP_PRE_DEC,
    UNOP_POST_INC, UNOP_POST_DEC,
} UnOpKind;

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_AND, BINOP_OR,  BINOP_XOR, BINOP_SHL, BINOP_SHR,
    BINOP_LAND, BINOP_LOR,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LE, BINOP_GE,
    BINOP_ASSIGN,
    BINOP_ADD_ASSIGN, BINOP_SUB_ASSIGN, BINOP_MUL_ASSIGN,
    BINOP_DIV_ASSIGN, BINOP_MOD_ASSIGN,
    BINOP_AND_ASSIGN, BINOP_OR_ASSIGN, BINOP_XOR_ASSIGN,
    BINOP_SHL_ASSIGN, BINOP_SHR_ASSIGN,
} BinOpKind;

typedef struct { StrView name; Expr *value; } FieldInit;

struct Expr {
    ExprKind   kind;
    SrcLoc     loc;
    TypeNode  *resolved_type;
    bool       is_lvalue;
    union {
        u64      int_val;
        f64      flt_val;
        StrView  str_val;
        bool     bool_val;
        struct { UnOpKind op; Expr *operand; }              unary;
        struct { BinOpKind op; Expr *lhs; Expr *rhs; }     binary;
        struct { Expr *callee; Expr **args; u32 arg_count; } call;
        struct { Expr *base; Expr *index; }                 subscript;
        struct { Expr *base; StrView field; }               field;
        struct { Expr *operand; }                           deref;
        struct { Expr *operand; bool is_mut; }              addr_of;
        struct { Expr *operand; TypeNode *to; }             cast;
        struct { TypeNode *type; }                          sizeof_expr;
        struct { TypeNode *type; FieldInit *fields; u32 field_count; } struct_init;
        struct { Expr **elems; u32 count; }                 array_init;
    };
};

typedef enum {
    STMT_BLOCK, STMT_EXPR, STMT_LET, STMT_CONST, STMT_RETURN,
    STMT_IF, STMT_WHILE, STMT_FOR, STMT_BREAK, STMT_CONTINUE,
    STMT_DEFER, STMT_ASSIGN, STMT_COUNT
} StmtKind;

struct Stmt {
    StmtKind kind;
    SrcLoc   loc;
    union {
        struct { Stmt **stmts; u32 count; }                         block;
        struct { Expr *expr; }                                       expr_stmt;
        struct { StrView name; TypeNode *type; Expr *init;
                 bool is_const; bool is_mut; }                      let;
        struct { Expr *value; }                                      ret;
        struct { Expr *cond; Stmt *then_body; Stmt *else_body; }    if_stmt;
        struct { Expr *cond; Stmt *body; }                          while_stmt;
        struct { Stmt *init; Expr *cond; Stmt *post; Stmt *body; }  for_stmt;
        struct { Stmt *deferred; }                                   defer_stmt;
    };
};

typedef enum {
    DECL_FUNC, DECL_STRUCT, DECL_UNION, DECL_ENUM,
    DECL_VAR, DECL_IMPORT, DECL_COUNT
} DeclKind;

typedef struct { StrView name; TypeNode *type; Expr *default_val; } ParamDecl;
typedef struct { StrView name; TypeNode *type; Expr *init; }        FieldDecl;
typedef struct { StrView name; i64 value; bool has_explicit_value; } EnumVariant;

struct Decl {
    DeclKind kind;
    SrcLoc   loc;
    StrView  name;
    bool     is_extern, is_inline, is_exported;
    union {
        struct { ParamDecl *params; u32 param_count;
                 TypeNode *ret_type; Stmt *body; bool variadic; } func;
        struct { FieldDecl *fields; u32 field_count; }            record;
        struct { EnumVariant *variants; u32 variant_count;
                 TypeNode *backing_type; }                         enum_decl;
        struct { TypeNode *type; Expr *init; }                     var;
        struct { StrView path; }                                    import;
    };
};

struct AstModule {
    const char *filename;
    Decl      **decls;
    u32         decl_count;
    Arena      *arena;
};
