// ============================================================
//  O Language Compiler — o_parser.c
//  Recursive-descent parser: tokens -> AST
//  Z-TEAM | C23
// ============================================================
#include "core/common.h"
#include "core/arena.h"
#include "frontend/lexer.h"
#include "frontend/ast.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct {
    Lexer     lex;
    Token     cur, peek;
    Arena    *arena;
    AstModule *module;
    bool      had_error;
    int       error_count;
} Parser;

static void COLD parse_error(Parser *p, const char *fmt, ...) {
    p->had_error = true; p->error_count++;
    fprintf(stderr, "\033[31merror\033[0m [%s:%u:%u]: ",
            p->cur.loc.file, p->cur.loc.line, p->cur.loc.col);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

static void    advance(Parser *p)           { p->cur = p->peek; p->peek = lexer_next(&p->lex); }
static bool    check(const Parser *p, TokKind k) { return p->cur.kind == k; }
static bool    check_peek(const Parser *p, TokKind k) { return p->peek.kind == k; }
static bool    match(Parser *p, TokKind k)  { if (!check(p,k)) return false; advance(p); return true; }

static Token expect(Parser *p, TokKind k, const char *what) {
    if (!check(p, k)) {
        parse_error(p, "expected %s, got '%.*s'", what, (int)p->cur.text.len, p->cur.text.ptr);
        return p->cur;
    }
    Token t = p->cur; advance(p); return t;
}

static Expr    *alloc_expr(Parser *p, ExprKind k, SrcLoc loc) { Expr *e=ARENA_ALLOC(p->arena,Expr); e->kind=k; e->loc=loc; e->resolved_type=NULL; e->is_lvalue=false; return e; }
static Stmt    *alloc_stmt(Parser *p, StmtKind k, SrcLoc loc) { Stmt *s=ARENA_ALLOC(p->arena,Stmt); s->kind=k; s->loc=loc; return s; }
static Decl    *alloc_decl(Parser *p, DeclKind k, SrcLoc loc) { Decl *d=ARENA_ALLOC(p->arena,Decl); d->kind=k; d->loc=loc; d->is_extern=false; d->is_inline=false; d->is_exported=false; return d; }
static TypeNode*alloc_type(Parser *p, TypeKind k, SrcLoc loc) { TypeNode *t=ARENA_ALLOC(p->arena,TypeNode); t->kind=k; t->loc=loc; return t; }

static TypeNode *parse_type(Parser *p) {
    SrcLoc loc = p->cur.loc;
    if (match(p, TOK_STAR)) {
        bool is_mut = match(p, TOK_KW_MUT);
        TypeNode *pointee = parse_type(p);
        TypeNode *t = alloc_type(p, TY_POINTER, loc);
        t->ptr.pointee = pointee; t->ptr.is_mut = is_mut; return t;
    }
    if (check(p, TOK_LBRACKET) && check_peek(p, TOK_RBRACKET)) {
        advance(p); advance(p);
        TypeNode *t = alloc_type(p, TY_SLICE, loc); t->slice.elem = parse_type(p); return t;
    }
    if (match(p, TOK_LBRACKET)) {
        Token ct = expect(p, TOK_INT_LIT, "array size"); expect(p, TOK_RBRACKET, "]");
        TypeNode *t = alloc_type(p, TY_ARRAY, loc); t->arr.elem = parse_type(p); t->arr.count = ct.int_val; return t;
    }
    if (match(p, TOK_KW_FN)) {
        expect(p, TOK_LPAREN, "(");
        TypeNode **params = arena_alloc(p->arena, sizeof(TypeNode*)*4, alignof(TypeNode*));
        u32 pc=0, pcap=4; bool variadic=false;
        while (!check(p,TOK_RPAREN)&&!check(p,TOK_EOF)) {
            if (match(p,TOK_DOTDOTDOT)){variadic=true;break;}
            if (pc==pcap){TypeNode**np=arena_alloc(p->arena,sizeof(TypeNode*)*pcap*2,alignof(TypeNode*));memcpy(np,params,sizeof(TypeNode*)*pc);params=np;pcap*=2;}
            params[pc++]=parse_type(p); if(!match(p,TOK_COMMA))break;
        }
        expect(p, TOK_RPAREN, ")");
        TypeNode *ret=NULL; if(match(p,TOK_ARROW)) ret=parse_type(p);
        TypeNode *t=alloc_type(p,TY_FUNC,loc); t->func.params=params; t->func.param_count=pc; t->func.ret=ret; t->func.variadic=variadic; return t;
    }
    static const struct{TokKind tok;TypeKind ty;} pm[]={
        {TOK_KW_VOID,TY_VOID},{TOK_KW_BOOL,TY_BOOL},
        {TOK_KW_I8,TY_I8},{TOK_KW_I16,TY_I16},{TOK_KW_I32,TY_I32},{TOK_KW_I64,TY_I64},
        {TOK_KW_U8,TY_U8},{TOK_KW_U16,TY_U16},{TOK_KW_U32,TY_U32},{TOK_KW_U64,TY_U64},
        {TOK_KW_F32,TY_F32},{TOK_KW_F64,TY_F64},{TOK_KW_ISIZE,TY_ISIZE},{TOK_KW_USIZE,TY_USIZE},
    };
    for (u32 i=0;i<ARRAY_LEN(pm);i++) if(match(p,pm[i].tok)) return alloc_type(p,pm[i].ty,loc);
    if (check(p,TOK_IDENT)){StrView name=p->cur.text;advance(p);TypeNode*t=alloc_type(p,TY_NAMED,loc);t->name=name;return t;}
    parse_error(p,"expected type, got '%.*s'",(int)p->cur.text.len,p->cur.text.ptr);
    return alloc_type(p,TY_VOID,loc);
}

static Expr *parse_expr(Parser *p);
static Expr *parse_assign(Parser *p);
static Expr *parse_logic_or(Parser *p);
static Expr *parse_logic_and(Parser *p);
static Expr *parse_bitwise_or(Parser *p);
static Expr *parse_bitwise_xor(Parser *p);
static Expr *parse_bitwise_and(Parser *p);
static Expr *parse_equality(Parser *p);
static Expr *parse_relational(Parser *p);
static Expr *parse_shift(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_multiplicative(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);

static Expr *parse_expr(Parser *p) { return parse_assign(p); }

static Expr *parse_assign(Parser *p) {
    Expr *lhs = parse_logic_or(p); SrcLoc loc = p->cur.loc;
    static const struct{TokKind tok;BinOpKind op;}aops[]={
        {TOK_ASSIGN,BINOP_ASSIGN},{TOK_PLUS_ASSIGN,BINOP_ADD_ASSIGN},{TOK_MINUS_ASSIGN,BINOP_SUB_ASSIGN},
        {TOK_STAR_ASSIGN,BINOP_MUL_ASSIGN},{TOK_SLASH_ASSIGN,BINOP_DIV_ASSIGN},{TOK_PERCENT_ASSIGN,BINOP_MOD_ASSIGN},
        {TOK_AMP_ASSIGN,BINOP_AND_ASSIGN},{TOK_PIPE_ASSIGN,BINOP_OR_ASSIGN},{TOK_CARET_ASSIGN,BINOP_XOR_ASSIGN},
        {TOK_LSHIFT_ASSIGN,BINOP_SHL_ASSIGN},{TOK_RSHIFT_ASSIGN,BINOP_SHR_ASSIGN},
    };
    for(u32 i=0;i<ARRAY_LEN(aops);i++){if(match(p,aops[i].tok)){Expr*rhs=parse_assign(p);Expr*e=alloc_expr(p,EXPR_BINARY,loc);e->binary.op=aops[i].op;e->binary.lhs=lhs;e->binary.rhs=rhs;return e;}}
    return lhs;
}

#define BINLEVEL(name, next, ...)                                              \
static Expr *name(Parser *p) {                                                 \
    Expr *lhs = next(p);                                                       \
    static const struct{TokKind tok;BinOpKind op;}ops[]={__VA_ARGS__};         \
    while(1){bool found=false;SrcLoc loc=p->cur.loc;                          \
        for(u32 i=0;i<ARRAY_LEN(ops);i++){if(match(p,ops[i].tok)){             \
            Expr*rhs=next(p);Expr*e=alloc_expr(p,EXPR_BINARY,loc);            \
            e->binary.op=ops[i].op;e->binary.lhs=lhs;e->binary.rhs=rhs;      \
            lhs=e;found=true;break;}}if(!found)break;}return lhs;}

BINLEVEL(parse_logic_or,    parse_logic_and,    {TOK_PIPE_PIPE,BINOP_LOR})
BINLEVEL(parse_logic_and,   parse_bitwise_or,   {TOK_AMP_AMP,BINOP_LAND})
BINLEVEL(parse_bitwise_or,  parse_bitwise_xor,  {TOK_PIPE,BINOP_OR})
BINLEVEL(parse_bitwise_xor, parse_bitwise_and,  {TOK_CARET,BINOP_XOR})
BINLEVEL(parse_bitwise_and, parse_equality,     {TOK_AMP,BINOP_AND})
BINLEVEL(parse_equality,    parse_relational,   {TOK_EQ,BINOP_EQ},{TOK_NEQ,BINOP_NEQ})
BINLEVEL(parse_relational,  parse_shift,        {TOK_LT,BINOP_LT},{TOK_GT,BINOP_GT},{TOK_LE,BINOP_LE},{TOK_GE,BINOP_GE})
BINLEVEL(parse_shift,       parse_additive,     {TOK_LSHIFT,BINOP_SHL},{TOK_RSHIFT,BINOP_SHR})
BINLEVEL(parse_additive,    parse_multiplicative,{TOK_PLUS,BINOP_ADD},{TOK_MINUS,BINOP_SUB})
BINLEVEL(parse_multiplicative,parse_unary,      {TOK_STAR,BINOP_MUL},{TOK_SLASH,BINOP_DIV},{TOK_PERCENT,BINOP_MOD})

static Expr *parse_unary(Parser *p) {
    SrcLoc loc=p->cur.loc;
    if(match(p,TOK_MINUS)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_NEG;e->unary.operand=parse_unary(p);return e;}
    if(match(p,TOK_BANG)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_NOT;e->unary.operand=parse_unary(p);return e;}
    if(match(p,TOK_TILDE)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_BNOT;e->unary.operand=parse_unary(p);return e;}
    if(match(p,TOK_PLUS_PLUS)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_PRE_INC;e->unary.operand=parse_unary(p);return e;}
    if(match(p,TOK_MINUS_MINUS)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_PRE_DEC;e->unary.operand=parse_unary(p);return e;}
    if(match(p,TOK_STAR)){Expr*e=alloc_expr(p,EXPR_DEREF,loc);e->deref.operand=parse_unary(p);return e;}
    if(match(p,TOK_AMP)){bool m_=match(p,TOK_KW_MUT);Expr*e=alloc_expr(p,EXPR_ADDR_OF,loc);e->addr_of.operand=parse_unary(p);e->addr_of.is_mut=m_;return e;}
    if(match(p,TOK_KW_SIZEOF)){expect(p,TOK_LPAREN,"(");TypeNode*ty=parse_type(p);expect(p,TOK_RPAREN,")");Expr*e=alloc_expr(p,EXPR_SIZEOF,loc);e->sizeof_expr.type=ty;return e;}
    if(match(p,TOK_KW_ALIGNOF)){expect(p,TOK_LPAREN,"(");TypeNode*ty=parse_type(p);expect(p,TOK_RPAREN,")");Expr*e=alloc_expr(p,EXPR_ALIGNOF,loc);e->sizeof_expr.type=ty;return e;}
    return parse_postfix(p);
}

static Expr *parse_postfix(Parser *p) {
    Expr *base = parse_primary(p);
    while(1) {
        SrcLoc loc=p->cur.loc;
        if(match(p,TOK_LPAREN)){
            Expr**args=arena_alloc(p->arena,sizeof(Expr*)*4,alignof(Expr*));
            u32 ac=0,acap=4;
            while(!check(p,TOK_RPAREN)&&!check(p,TOK_EOF)){
                if(ac==acap){Expr**na=arena_alloc(p->arena,sizeof(Expr*)*acap*2,alignof(Expr*));memcpy(na,args,sizeof(Expr*)*ac);args=na;acap*=2;}
                args[ac++]=parse_expr(p); if(!match(p,TOK_COMMA))break;
            }
            expect(p,TOK_RPAREN,")");
            Expr*e=alloc_expr(p,EXPR_CALL,loc);e->call.callee=base;e->call.args=args;e->call.arg_count=ac;base=e;continue;
        }
        if(match(p,TOK_LBRACKET)){Expr*idx=parse_expr(p);expect(p,TOK_RBRACKET,"]");Expr*e=alloc_expr(p,EXPR_INDEX,loc);e->subscript.base=base;e->subscript.index=idx;base=e;continue;}
        if(match(p,TOK_DOT)){Token ft=expect(p,TOK_IDENT,"field name");Expr*e=alloc_expr(p,EXPR_FIELD,loc);e->field.base=base;e->field.field=ft.text;base=e;continue;}
        if(match(p,TOK_PLUS_PLUS)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_POST_INC;e->unary.operand=base;base=e;continue;}
        if(match(p,TOK_MINUS_MINUS)){Expr*e=alloc_expr(p,EXPR_UNARY,loc);e->unary.op=UNOP_POST_DEC;e->unary.operand=base;base=e;continue;}
        if(match(p,TOK_KW_AS)){TypeNode*to=parse_type(p);Expr*e=alloc_expr(p,EXPR_CAST,loc);e->cast.operand=base;e->cast.to=to;base=e;continue;}
        break;
    }
    return base;
}

static Expr *parse_primary(Parser *p) {
    SrcLoc loc=p->cur.loc;
    if(check(p,TOK_INT_LIT)){Expr*e=alloc_expr(p,EXPR_INT_LIT,loc);e->int_val=p->cur.int_val;advance(p);return e;}
    if(check(p,TOK_FLOAT_LIT)){Expr*e=alloc_expr(p,EXPR_FLOAT_LIT,loc);e->flt_val=p->cur.flt_val;advance(p);return e;}
    if(check(p,TOK_STRING_LIT)){Expr*e=alloc_expr(p,EXPR_STRING_LIT,loc);e->str_val=p->cur.text;advance(p);return e;}
    if(check(p,TOK_CHAR_LIT)){Expr*e=alloc_expr(p,EXPR_CHAR_LIT,loc);e->int_val=p->cur.int_val;advance(p);return e;}
    if(match(p,TOK_KW_TRUE)){Expr*e=alloc_expr(p,EXPR_BOOL_LIT,loc);e->bool_val=true;return e;}
    if(match(p,TOK_KW_FALSE)){Expr*e=alloc_expr(p,EXPR_BOOL_LIT,loc);e->bool_val=false;return e;}
    if(match(p,TOK_KW_NULL)) return alloc_expr(p,EXPR_NULL_LIT,loc);
    if(check(p,TOK_IDENT)){
        StrView name=p->cur.text; advance(p);
        if(check(p,TOK_LBRACE)&&check_peek(p,TOK_DOT)){
            advance(p);
            FieldInit*fields=arena_alloc(p->arena,sizeof(FieldInit)*4,alignof(FieldInit));
            u32 fc=0,fcap=4;
            while(!check(p,TOK_RBRACE)&&!check(p,TOK_EOF)){
                if(fc==fcap){FieldInit*nf=arena_alloc(p->arena,sizeof(FieldInit)*fcap*2,alignof(FieldInit));memcpy(nf,fields,sizeof(FieldInit)*fc);fields=nf;fcap*=2;}
                expect(p,TOK_DOT,".");Token fn_t=expect(p,TOK_IDENT,"field name");expect(p,TOK_ASSIGN,"=");
                Expr*val=parse_expr(p);fields[fc++]=(FieldInit){.name=fn_t.text,.value=val};if(!match(p,TOK_COMMA))break;
            }
            expect(p,TOK_RBRACE,"}");
            Expr*e=alloc_expr(p,EXPR_STRUCT_INIT,loc);TypeNode*nt=alloc_type(p,TY_NAMED,loc);nt->name=name;
            e->struct_init.type=nt;e->struct_init.fields=fields;e->struct_init.field_count=fc;return e;
        }
        Expr*e=alloc_expr(p,EXPR_IDENT,loc);e->str_val=name;return e;
    }
    if(match(p,TOK_LBRACKET)){
        Expr**elems=arena_alloc(p->arena,sizeof(Expr*)*4,alignof(Expr*));u32 ec=0,ecap=4;
        while(!check(p,TOK_RBRACKET)&&!check(p,TOK_EOF)){
            if(ec==ecap){Expr**ne=arena_alloc(p->arena,sizeof(Expr*)*ecap*2,alignof(Expr*));memcpy(ne,elems,sizeof(Expr*)*ec);elems=ne;ecap*=2;}
            elems[ec++]=parse_expr(p);if(!match(p,TOK_COMMA))break;
        }
        expect(p,TOK_RBRACKET,"]");Expr*e=alloc_expr(p,EXPR_ARRAY_INIT,loc);e->array_init.elems=elems;e->array_init.count=ec;return e;
    }
    if(match(p,TOK_LPAREN)){Expr*e=parse_expr(p);expect(p,TOK_RPAREN,")");return e;}
    parse_error(p,"unexpected token in expression: '%.*s'",(int)p->cur.text.len,p->cur.text.ptr);
    Expr*e=alloc_expr(p,EXPR_INT_LIT,loc);e->int_val=0;advance(p);return e;
}

static Stmt *parse_stmt(Parser *p);

static Stmt *parse_block(Parser *p) {
    SrcLoc loc=p->cur.loc; expect(p,TOK_LBRACE,"{");
    Stmt**stmts=arena_alloc(p->arena,sizeof(Stmt*)*8,alignof(Stmt*));u32 sc=0,scap=8;
    while(!check(p,TOK_RBRACE)&&!check(p,TOK_EOF)){
        if(sc==scap){Stmt**ns=arena_alloc(p->arena,sizeof(Stmt*)*scap*2,alignof(Stmt*));memcpy(ns,stmts,sizeof(Stmt*)*sc);stmts=ns;scap*=2;}
        stmts[sc++]=parse_stmt(p);
    }
    expect(p,TOK_RBRACE,"}");Stmt*s=alloc_stmt(p,STMT_BLOCK,loc);s->block.stmts=stmts;s->block.count=sc;return s;
}

static Stmt *parse_stmt(Parser *p) {
    SrcLoc loc=p->cur.loc;
    if(check(p,TOK_LBRACE)) return parse_block(p);
    if(check(p,TOK_KW_LET)||check(p,TOK_KW_CONST)){
        bool is_const=check(p,TOK_KW_CONST); advance(p);
        bool is_mut=match(p,TOK_KW_MUT);
        Token nt=expect(p,TOK_IDENT,"variable name");
        TypeNode*ty=NULL; if(match(p,TOK_COLON)) ty=parse_type(p);
        Expr*init=NULL; if(match(p,TOK_ASSIGN)) init=parse_expr(p);
        else if(!ty) parse_error(p,"let binding requires type or initializer");
        expect(p,TOK_SEMI,";");
        Stmt*s=alloc_stmt(p,STMT_LET,loc);s->let.name=nt.text;s->let.type=ty;s->let.init=init;s->let.is_const=is_const;s->let.is_mut=is_mut;return s;
    }
    if(match(p,TOK_KW_RETURN)){Expr*v=NULL;if(!check(p,TOK_SEMI))v=parse_expr(p);expect(p,TOK_SEMI,";");Stmt*s=alloc_stmt(p,STMT_RETURN,loc);s->ret.value=v;return s;}
    if(match(p,TOK_KW_IF)){
        Expr*cond=parse_expr(p);Stmt*then=parse_block(p);Stmt*els=NULL;
        if(match(p,TOK_KW_ELSE)){if(check(p,TOK_KW_IF))els=parse_stmt(p);else els=parse_block(p);}
        Stmt*s=alloc_stmt(p,STMT_IF,loc);s->if_stmt.cond=cond;s->if_stmt.then_body=then;s->if_stmt.else_body=els;return s;
    }
    if(match(p,TOK_KW_WHILE)){Expr*c=parse_expr(p);Stmt*b=parse_block(p);Stmt*s=alloc_stmt(p,STMT_WHILE,loc);s->while_stmt.cond=c;s->while_stmt.body=b;return s;}
    if(match(p,TOK_KW_FOR)){
        Stmt*init_s=NULL;Expr*cond=NULL;Stmt*post_s=NULL;
        if(!check(p,TOK_SEMI))init_s=parse_stmt(p);else advance(p);
        if(!check(p,TOK_SEMI))cond=parse_expr(p);
        expect(p,TOK_SEMI,";");
        if(!check(p,TOK_LBRACE)){Expr*pe=parse_expr(p);Stmt*ps=alloc_stmt(p,STMT_EXPR,loc);ps->expr_stmt.expr=pe;post_s=ps;}
        Stmt*body=parse_block(p);Stmt*s=alloc_stmt(p,STMT_FOR,loc);
        s->for_stmt.init=init_s;s->for_stmt.cond=cond;s->for_stmt.post=post_s;s->for_stmt.body=body;return s;
    }
    if(match(p,TOK_KW_BREAK)){expect(p,TOK_SEMI,";");return alloc_stmt(p,STMT_BREAK,loc);}
    if(match(p,TOK_KW_CONTINUE)){expect(p,TOK_SEMI,";");return alloc_stmt(p,STMT_CONTINUE,loc);}
    if(match(p,TOK_KW_DEFER)){Stmt*d=parse_stmt(p);Stmt*s=alloc_stmt(p,STMT_DEFER,loc);s->defer_stmt.deferred=d;return s;}
    Expr*e=parse_expr(p);expect(p,TOK_SEMI,";");Stmt*s=alloc_stmt(p,STMT_EXPR,loc);s->expr_stmt.expr=e;return s;
}

static Decl *parse_decl(Parser *p) {
    SrcLoc loc=p->cur.loc;
    bool is_extern=match(p,TOK_KW_EXTERN);
    bool is_inline=match(p,TOK_KW_INLINE);
    if(match(p,TOK_KW_FN)){
        Token nt=expect(p,TOK_IDENT,"function name"); expect(p,TOK_LPAREN,"(");
        ParamDecl*params=arena_alloc(p->arena,sizeof(ParamDecl)*4,alignof(ParamDecl));
        u32 pc=0,pcap=4; bool variadic=false;
        while(!check(p,TOK_RPAREN)&&!check(p,TOK_EOF)){
            if(match(p,TOK_DOTDOTDOT)){variadic=true;break;}
            if(pc==pcap){ParamDecl*np=arena_alloc(p->arena,sizeof(ParamDecl)*pcap*2,alignof(ParamDecl));memcpy(np,params,sizeof(ParamDecl)*pc);params=np;pcap*=2;}
            Token pn=expect(p,TOK_IDENT,"param name");expect(p,TOK_COLON,":");TypeNode*pty=parse_type(p);
            params[pc++]=(ParamDecl){.name=pn.text,.type=pty,.default_val=NULL};if(!match(p,TOK_COMMA))break;
        }
        expect(p,TOK_RPAREN,")");
        TypeNode*ret=NULL; if(match(p,TOK_ARROW)) ret=parse_type(p);
        Stmt*body=NULL; if(check(p,TOK_LBRACE)) body=parse_block(p); else expect(p,TOK_SEMI,";");
        Decl*d=alloc_decl(p,DECL_FUNC,loc);d->name=nt.text;d->is_extern=is_extern;d->is_inline=is_inline;
        d->func.params=params;d->func.param_count=pc;d->func.ret_type=ret;d->func.body=body;d->func.variadic=variadic;return d;
    }
    if(match(p,TOK_KW_STRUCT)){
        Token nt=expect(p,TOK_IDENT,"struct name"); expect(p,TOK_LBRACE,"{");
        FieldDecl*fields=arena_alloc(p->arena,sizeof(FieldDecl)*4,alignof(FieldDecl));u32 fc=0,fcap=4;
        while(!check(p,TOK_RBRACE)&&!check(p,TOK_EOF)){
            if(fc==fcap){FieldDecl*nf=arena_alloc(p->arena,sizeof(FieldDecl)*fcap*2,alignof(FieldDecl));memcpy(nf,fields,sizeof(FieldDecl)*fc);fields=nf;fcap*=2;}
            Token fn_t=expect(p,TOK_IDENT,"field name");expect(p,TOK_COLON,":");TypeNode*fty=parse_type(p);
            fields[fc++]=(FieldDecl){.name=fn_t.text,.type=fty,.init=NULL};match(p,TOK_COMMA);if(check(p,TOK_SEMI))advance(p);
        }
        expect(p,TOK_RBRACE,"}");Decl*d=alloc_decl(p,DECL_STRUCT,loc);d->name=nt.text;d->record.fields=fields;d->record.field_count=fc;return d;
    }
    if(match(p,TOK_KW_ENUM)){
        Token nt=expect(p,TOK_IDENT,"enum name");TypeNode*backing=NULL;if(match(p,TOK_COLON))backing=parse_type(p);
        expect(p,TOK_LBRACE,"{");EnumVariant*variants=arena_alloc(p->arena,sizeof(EnumVariant)*4,alignof(EnumVariant));
        u32 vc=0,vcap=4;i64 auto_val=0;
        while(!check(p,TOK_RBRACE)&&!check(p,TOK_EOF)){
            if(vc==vcap){EnumVariant*nv=arena_alloc(p->arena,sizeof(EnumVariant)*vcap*2,alignof(EnumVariant));memcpy(nv,variants,sizeof(EnumVariant)*vc);variants=nv;vcap*=2;}
            Token vn=expect(p,TOK_IDENT,"enum variant");bool has_exp=false;i64 val=auto_val;
            if(match(p,TOK_ASSIGN)){Token vv=expect(p,TOK_INT_LIT,"enum value");val=(i64)vv.int_val;has_exp=true;}
            auto_val=val+1;variants[vc++]=(EnumVariant){.name=vn.text,.value=val,.has_explicit_value=has_exp};match(p,TOK_COMMA);
        }
        expect(p,TOK_RBRACE,"}");Decl*d=alloc_decl(p,DECL_ENUM,loc);d->name=nt.text;
        d->enum_decl.variants=variants;d->enum_decl.variant_count=vc;d->enum_decl.backing_type=backing;return d;
    }
    if(match(p,TOK_KW_IMPORT)){Token pt=expect(p,TOK_STRING_LIT,"import path");expect(p,TOK_SEMI,";");Decl*d=alloc_decl(p,DECL_IMPORT,loc);d->import.path=pt.text;return d;}
    if(check(p,TOK_KW_LET)||check(p,TOK_KW_CONST)){
        advance(p);Token nt=expect(p,TOK_IDENT,"var name");TypeNode*ty=NULL;if(match(p,TOK_COLON))ty=parse_type(p);
        Expr*init=NULL;if(match(p,TOK_ASSIGN))init=parse_expr(p);expect(p,TOK_SEMI,";");
        Decl*d=alloc_decl(p,DECL_VAR,loc);d->name=nt.text;d->is_extern=is_extern;d->var.type=ty;d->var.init=init;return d;
    }
    parse_error(p,"expected declaration, got '%.*s'",(int)p->cur.text.len,p->cur.text.ptr);
    advance(p); return NULL;
}

AstModule *parse_source(const char *source, const char *filename, Arena *arena) {
    Parser p={0}; p.arena=arena;
    lexer_init(&p.lex,arena,filename,source,strlen(source));
    p.cur=lexer_next(&p.lex); p.peek=lexer_next(&p.lex);
    AstModule*m=ARENA_ALLOC(arena,AstModule); m->filename=filename; m->arena=arena; m->decl_count=0;
    u32 dcap=32; m->decls=arena_alloc(arena,sizeof(Decl*)*dcap,alignof(Decl*));
    while(!check(&p,TOK_EOF)){
        if(m->decl_count==dcap){Decl**nd=arena_alloc(arena,sizeof(Decl*)*dcap*2,alignof(Decl*));memcpy(nd,m->decls,sizeof(Decl*)*m->decl_count);m->decls=nd;dcap*=2;}
        Decl*d=parse_decl(&p); if(d) m->decls[m->decl_count++]=d;
    }
    if(p.had_error) fprintf(stderr,"\033[31m%d parse error(s) in %s\033[0m\n",p.error_count,filename);
    return m;
}
