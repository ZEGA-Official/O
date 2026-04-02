// ============================================================
//  O Language Compiler -- o_sema.c
//  Semantic analysis: name resolution, type inference, type checking
//  Z-TEAM | C23
// ============================================================
#include "core/common.h"
#include "core/arena.h"
#include "frontend/ast.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef enum { SYM_VAR, SYM_FUNC, SYM_TYPE, SYM_PARAM } SymKind;

typedef struct Symbol Symbol;
struct Symbol {
    StrView   name;
    SymKind   kind;
    TypeNode *type;
    Decl     *decl;
    Symbol   *next;
};

#define SCOPE_HASH 64

typedef struct Scope Scope;
struct Scope {
    Symbol *buckets[SCOPE_HASH];
    Scope  *parent;
    Arena  *arena;
};

static Scope *scope_new(Arena *arena, Scope *parent) {
    Scope *s = ARENA_ALLOC(arena, Scope);
    s->parent = parent; s->arena = arena;
    memset(s->buckets, 0, sizeof(s->buckets));
    return s;
}

static u32 str_hash(StrView sv) {
    u32 h = 2166136261u;
    for (u32 i=0; i<sv.len; i++) { h ^= (u8)sv.ptr[i]; h *= 16777619u; }
    return h;
}

static void scope_define(Scope *s, StrView name, SymKind kind, TypeNode *type, Decl *decl) {
    u32 idx = str_hash(name) % SCOPE_HASH;
    Symbol *sym = ARENA_ALLOC(s->arena, Symbol);
    sym->name=name; sym->kind=kind; sym->type=type; sym->decl=decl;
    sym->next=s->buckets[idx]; s->buckets[idx]=sym;
}

static Symbol *scope_lookup(const Scope *s, StrView name) {
    for (const Scope *cur=s; cur; cur=cur->parent) {
        u32 idx=str_hash(name)%SCOPE_HASH;
        for (Symbol *sym=cur->buckets[idx]; sym; sym=sym->next)
            if (sym->name.len==name.len && memcmp(sym->name.ptr,name.ptr,name.len)==0)
                return sym;
    }
    return NULL;
}

typedef struct {
    Scope    *scope;
    Arena    *arena;
    bool      had_error;
    int       error_count;
    TypeNode *current_ret_type;
    bool      in_loop;
} SemaCtx;

static void COLD sema_error(SemaCtx *ctx, SrcLoc loc, const char *fmt, ...) {
    ctx->had_error=true; ctx->error_count++;
    fprintf(stderr,"\033[31merror\033[0m [%s:%u:%u]: ",loc.file,loc.line,loc.col);
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
    fprintf(stderr,"\n");
}

static TypeNode *make_prim(SemaCtx *ctx, TypeKind k) {
    TypeNode *t=ARENA_ALLOC(ctx->arena,TypeNode); t->kind=k; t->loc=srcloc_invalid(); return t;
}

static bool types_equal(const TypeNode *a, const TypeNode *b) {
    if (!a||!b) return a==b;
    if (a->kind!=b->kind) return false;
    switch(a->kind){
        case TY_POINTER: return types_equal(a->ptr.pointee,b->ptr.pointee);
        case TY_ARRAY: return a->arr.count==b->arr.count&&types_equal(a->arr.elem,b->arr.elem);
        case TY_NAMED: return a->name.len==b->name.len&&memcmp(a->name.ptr,b->name.ptr,a->name.len)==0;
        default: return true;
    }
}

static bool type_is_integral(const TypeNode *t)      { return t&&ty_is_integer(t->kind); }
static bool type_is_numeric_node(const TypeNode *t)  { return t&&ty_is_numeric(t->kind); }

static TypeNode *sema_expr(SemaCtx *ctx, Expr *e);
static void      sema_stmt(SemaCtx *ctx, Stmt *s);
static TypeNode *sema_decl(SemaCtx *ctx, Decl *d);

static TypeNode *sema_expr(SemaCtx *ctx, Expr *e) {
    if (!e) return make_prim(ctx,TY_VOID);
    switch(e->kind) {
        case EXPR_INT_LIT:   e->resolved_type=make_prim(ctx,TY_I64);  return e->resolved_type;
        case EXPR_FLOAT_LIT: e->resolved_type=make_prim(ctx,TY_F64);  return e->resolved_type;
        case EXPR_BOOL_LIT:  e->resolved_type=make_prim(ctx,TY_BOOL); return e->resolved_type;
        case EXPR_NULL_LIT:  e->resolved_type=make_prim(ctx,TY_POINTER); return e->resolved_type;
        case EXPR_CHAR_LIT:  e->resolved_type=make_prim(ctx,TY_I8);   return e->resolved_type;
        case EXPR_STRING_LIT: {
            TypeNode *t=ARENA_ALLOC(ctx->arena,TypeNode);
            t->kind=TY_POINTER; t->loc=e->loc;
            t->ptr.pointee=make_prim(ctx,TY_U8); t->ptr.is_mut=false;
            e->resolved_type=t; return t;
        }
        case EXPR_IDENT: {
            Symbol *sym=scope_lookup(ctx->scope,e->str_val);
            if (!sym) {
                sema_error(ctx,e->loc,"undefined identifier '%.*s'",(int)e->str_val.len,e->str_val.ptr);
                e->resolved_type=make_prim(ctx,TY_VOID);
            } else {
                e->resolved_type=sym->type;
                e->is_lvalue=(sym->kind==SYM_VAR||sym->kind==SYM_PARAM);
            }
            return e->resolved_type;
        }
        case EXPR_UNARY: {
            TypeNode *oty=sema_expr(ctx,e->unary.operand);
            switch(e->unary.op){
                case UNOP_NEG: if(!type_is_numeric_node(oty)) sema_error(ctx,e->loc,"unary '-' needs numeric"); e->resolved_type=oty; break;
                case UNOP_NOT: e->resolved_type=make_prim(ctx,TY_BOOL); break;
                case UNOP_BNOT: if(!type_is_integral(oty)) sema_error(ctx,e->loc,"'~' needs integer"); e->resolved_type=oty; break;
                default: e->resolved_type=oty; break;
            }
            return e->resolved_type;
        }
        case EXPR_BINARY: {
            TypeNode *lty=sema_expr(ctx,e->binary.lhs);
            TypeNode *rty=sema_expr(ctx,e->binary.rhs);
            UNUSED(rty);
            switch(e->binary.op){
                case BINOP_ADD: case BINOP_SUB: case BINOP_MUL:
                case BINOP_DIV: case BINOP_MOD:
                    if(!type_is_numeric_node(lty)) sema_error(ctx,e->loc,"arithmetic needs numeric");
                    e->resolved_type=lty; break;
                case BINOP_AND: case BINOP_OR: case BINOP_XOR:
                case BINOP_SHL: case BINOP_SHR:
                    if(!type_is_integral(lty)) sema_error(ctx,e->loc,"bitwise needs integer");
                    e->resolved_type=lty; break;
                case BINOP_LAND: case BINOP_LOR:
                    e->resolved_type=make_prim(ctx,TY_BOOL); break;
                case BINOP_EQ: case BINOP_NEQ: case BINOP_LT:
                case BINOP_GT: case BINOP_LE:  case BINOP_GE:
                    e->resolved_type=make_prim(ctx,TY_BOOL); break;
                case BINOP_ASSIGN:
                    if(!e->binary.lhs->is_lvalue) sema_error(ctx,e->loc,"lvalue required for assignment");
                    e->resolved_type=lty; e->is_lvalue=false; break;
                default:
                    e->resolved_type=lty; break;
            }
            if(!e->resolved_type) e->resolved_type=lty;
            return e->resolved_type;
        }
        case EXPR_CALL: {
            TypeNode *cty=sema_expr(ctx,e->call.callee);
            for(u32 i=0;i<e->call.arg_count;i++) sema_expr(ctx,e->call.args[i]);
            if(cty&&cty->kind==TY_FUNC)
                e->resolved_type=cty->func.ret?cty->func.ret:make_prim(ctx,TY_VOID);
            else
                e->resolved_type=make_prim(ctx,TY_I64);
            return e->resolved_type;
        }
        case EXPR_INDEX: {
            TypeNode *bty=sema_expr(ctx,e->subscript.base);
            sema_expr(ctx,e->subscript.index);
            if(bty){
                if(bty->kind==TY_POINTER) e->resolved_type=bty->ptr.pointee;
                else if(bty->kind==TY_ARRAY) e->resolved_type=bty->arr.elem;
                else { sema_error(ctx,e->loc,"cannot index non-pointer/array"); e->resolved_type=make_prim(ctx,TY_VOID); }
            } else e->resolved_type=make_prim(ctx,TY_VOID);
            e->is_lvalue=true; return e->resolved_type;
        }
        case EXPR_FIELD:
            sema_expr(ctx,e->field.base);
            e->resolved_type=make_prim(ctx,TY_I64); e->is_lvalue=true; return e->resolved_type;
        case EXPR_DEREF: {
            TypeNode *pty=sema_expr(ctx,e->deref.operand);
            if(pty&&pty->kind==TY_POINTER) e->resolved_type=pty->ptr.pointee;
            else { sema_error(ctx,e->loc,"deref of non-pointer"); e->resolved_type=make_prim(ctx,TY_VOID); }
            e->is_lvalue=true; return e->resolved_type;
        }
        case EXPR_ADDR_OF: {
            TypeNode *inner=sema_expr(ctx,e->addr_of.operand);
            if(!e->addr_of.operand->is_lvalue) sema_error(ctx,e->loc,"cannot take address of non-lvalue");
            TypeNode *t=ARENA_ALLOC(ctx->arena,TypeNode);
            t->kind=TY_POINTER; t->loc=e->loc; t->ptr.pointee=inner; t->ptr.is_mut=e->addr_of.is_mut;
            e->resolved_type=t; return t;
        }
        case EXPR_CAST:
            sema_expr(ctx,e->cast.operand);
            e->resolved_type=e->cast.to; return e->cast.to;
        case EXPR_SIZEOF: case EXPR_ALIGNOF:
            e->resolved_type=make_prim(ctx,TY_USIZE); return e->resolved_type;
        case EXPR_STRUCT_INIT:
            for(u32 i=0;i<e->struct_init.field_count;i++) sema_expr(ctx,e->struct_init.fields[i].value);
            e->resolved_type=e->struct_init.type; return e->resolved_type;
        case EXPR_ARRAY_INIT: {
            TypeNode *ety=NULL;
            for(u32 i=0;i<e->array_init.count;i++){TypeNode *et=sema_expr(ctx,e->array_init.elems[i]);if(!ety)ety=et;}
            TypeNode *t=ARENA_ALLOC(ctx->arena,TypeNode); t->kind=TY_ARRAY; t->loc=e->loc;
            t->arr.elem=ety?ety:make_prim(ctx,TY_VOID); t->arr.count=e->array_init.count;
            e->resolved_type=t; return t;
        }
        default: e->resolved_type=make_prim(ctx,TY_VOID); return e->resolved_type;
    }
}

static void sema_stmt(SemaCtx *ctx, Stmt *s) {
    if (!s) return;
    switch(s->kind){
        case STMT_BLOCK: {
            Scope *saved=ctx->scope; ctx->scope=scope_new(ctx->arena,saved);
            for(u32 i=0;i<s->block.count;i++) sema_stmt(ctx,s->block.stmts[i]);
            ctx->scope=saved; break;
        }
        case STMT_LET: {
            TypeNode *ity=s->let.init?sema_expr(ctx,s->let.init):NULL;
            TypeNode *ty=s->let.type?s->let.type:ity;
            if(!ty){sema_error(ctx,s->loc,"cannot infer type of '%.*s'",(int)s->let.name.len,s->let.name.ptr);ty=make_prim(ctx,TY_VOID);}
            scope_define(ctx->scope,s->let.name,SYM_VAR,ty,NULL); break;
        }
        case STMT_EXPR: sema_expr(ctx,s->expr_stmt.expr); break;
        case STMT_RETURN: {
            TypeNode *rty=s->ret.value?sema_expr(ctx,s->ret.value):make_prim(ctx,TY_VOID);
            UNUSED(rty); break;
        }
        case STMT_IF:
            sema_expr(ctx,s->if_stmt.cond);
            sema_stmt(ctx,s->if_stmt.then_body);
            if(s->if_stmt.else_body) sema_stmt(ctx,s->if_stmt.else_body);
            break;
        case STMT_WHILE: {
            bool sl=ctx->in_loop; ctx->in_loop=true;
            sema_expr(ctx,s->while_stmt.cond); sema_stmt(ctx,s->while_stmt.body);
            ctx->in_loop=sl; break;
        }
        case STMT_FOR: {
            Scope *saved=ctx->scope; ctx->scope=scope_new(ctx->arena,saved);
            bool sl=ctx->in_loop; ctx->in_loop=true;
            sema_stmt(ctx,s->for_stmt.init);
            if(s->for_stmt.cond) sema_expr(ctx,s->for_stmt.cond);
            if(s->for_stmt.post) sema_stmt(ctx,s->for_stmt.post);
            sema_stmt(ctx,s->for_stmt.body);
            ctx->in_loop=sl; ctx->scope=saved; break;
        }
        case STMT_BREAK: case STMT_CONTINUE:
            if(!ctx->in_loop) sema_error(ctx,s->loc,"break/continue outside loop");
            break;
        case STMT_DEFER: sema_stmt(ctx,s->defer_stmt.deferred); break;
        default: break;
    }
}

static TypeNode *sema_decl(SemaCtx *ctx, Decl *d) {
    switch(d->kind){
        case DECL_FUNC: {
            TypeNode *ft=ARENA_ALLOC(ctx->arena,TypeNode);
            ft->kind=TY_FUNC; ft->loc=d->loc;
            ft->func.param_count=d->func.param_count;
            ft->func.ret=d->func.ret_type; ft->func.variadic=d->func.variadic;
            ft->func.params=arena_alloc(ctx->arena,sizeof(TypeNode*)*d->func.param_count,alignof(TypeNode*));
            for(u32 i=0;i<d->func.param_count;i++) ft->func.params[i]=d->func.params[i].type;
            scope_define(ctx->scope,d->name,SYM_FUNC,ft,d);
            if(!d->is_extern&&d->func.body){
                Scope *ss=ctx->scope; TypeNode *sr=ctx->current_ret_type;
                ctx->scope=scope_new(ctx->arena,ss); ctx->current_ret_type=d->func.ret_type;
                for(u32 i=0;i<d->func.param_count;i++)
                    scope_define(ctx->scope,d->func.params[i].name,SYM_PARAM,d->func.params[i].type,NULL);
                sema_stmt(ctx,d->func.body);
                ctx->scope=ss; ctx->current_ret_type=sr;
            }
            return ft;
        }
        case DECL_STRUCT: {
            TypeNode *ty=ARENA_ALLOC(ctx->arena,TypeNode); ty->kind=TY_NAMED; ty->name=d->name;
            scope_define(ctx->scope,d->name,SYM_TYPE,ty,d); return ty;
        }
        case DECL_ENUM: {
            TypeNode *backing=d->enum_decl.backing_type?d->enum_decl.backing_type:make_prim(ctx,TY_I32);
            TypeNode *ty=ARENA_ALLOC(ctx->arena,TypeNode); ty->kind=TY_NAMED; ty->name=d->name;
            scope_define(ctx->scope,d->name,SYM_TYPE,ty,d);
            for(u32 i=0;i<d->enum_decl.variant_count;i++)
                scope_define(ctx->scope,d->enum_decl.variants[i].name,SYM_VAR,backing,d);
            return ty;
        }
        case DECL_VAR: {
            TypeNode *ty=d->var.type;
            if(d->var.init){TypeNode *it=sema_expr(ctx,d->var.init);if(!ty)ty=it;}
            scope_define(ctx->scope,d->name,SYM_VAR,ty,d); return ty;
        }
        default: break;
    }
    return NULL;
}

typedef struct { bool had_error; int error_count; } SemaResult;

SemaResult sema_module(AstModule *m, Arena *arena) {
    SemaCtx ctx = {.arena=arena,.had_error=false,.error_count=0,.current_ret_type=NULL,.in_loop=false};
    ctx.scope = scope_new(arena, NULL);

    static const struct { const char *name; TypeKind ret; } builtins[] = {
        {"printf",TY_I32},{"puts",TY_I32},{"putchar",TY_I32},
        {"malloc",TY_POINTER},{"free",TY_VOID},{"exit",TY_VOID},
        {"strlen",TY_USIZE},{"memcpy",TY_POINTER},{"memset",TY_POINTER},
        {"scanf",TY_I32},{"fprintf",TY_I32},
    };
    for (u32 i=0; i<ARRAY_LEN(builtins); i++) {
        StrView name={.ptr=builtins[i].name,.len=strlen(builtins[i].name)};
        TypeNode *ft=ARENA_ALLOC(arena,TypeNode);
        ft->kind=TY_FUNC; ft->func.variadic=true; ft->func.param_count=0;
        ft->func.params=NULL; ft->func.ret=make_prim(&ctx,builtins[i].ret);
        scope_define(ctx.scope,name,SYM_FUNC,ft,NULL);
    }

    // Pass 1: externs + types
    for (u32 i=0; i<m->decl_count; i++) {
        Decl *d=m->decls[i]; if(!d) continue;
        if((d->kind==DECL_FUNC&&d->is_extern)||d->kind==DECL_STRUCT||d->kind==DECL_ENUM)
            sema_decl(&ctx,d);
    }
    // Pass 2: everything
    for (u32 i=0; i<m->decl_count; i++)
        if(m->decls[i]) sema_decl(&ctx,m->decls[i]);

    if(ctx.had_error)
        fprintf(stderr,"\033[31m%d semantic error(s) in %s\033[0m\n",ctx.error_count,m->filename);

    return (SemaResult){.had_error=ctx.had_error,.error_count=ctx.error_count};
}
