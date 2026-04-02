// ============================================================
//  O Language Compiler -- o_codegen.c
//  AST -> Three-address IR lowering
//  Z-TEAM | C23
// ============================================================
#include "core/common.h"
#include "core/arena.h"
#include "frontend/ast.h"
#include "ir/ir.h"
#include <string.h>
#include <stdio.h>

#define MAX_LOCALS 512
#define MAX_DEFERS 32

typedef struct { StrView name; IRVal ptr; TypeKind type; } LocalVar;

typedef struct CGCtx CGCtx;
struct CGCtx {
    IRModule *module; IRFunc *fn; IRBlock *cur_block; Arena *arena;
    LocalVar  locals[MAX_LOCALS]; u32 local_count;
    u32 break_labels[32]; u32 continue_labels[32]; u32 loop_depth;
    Stmt *defers[MAX_DEFERS]; u32 defer_count;
};

static IRBlock *cg_new_block(CGCtx *ctx, const char *name) {
    return ir_block_new(ctx->fn, (StrView){.ptr=name,.len=strlen(name)});
}
static void cg_switch_block(CGCtx *ctx, IRBlock *b) { ctx->cur_block=b; }

static LocalVar *cg_find_local(CGCtx *ctx, StrView name) {
    for(u32 i=0;i<ctx->local_count;i++)
        if(ctx->locals[i].name.len==name.len&&memcmp(ctx->locals[i].name.ptr,name.ptr,name.len)==0)
            return &ctx->locals[i];
    return NULL;
}

static TypeKind cg_typekind(const TypeNode *t) { return t?t->kind:TY_I64; }

static IRVal cg_expr(CGCtx *ctx, Expr *e);
static void  cg_stmt(CGCtx *ctx, Stmt *s);

static IRVal cg_expr(CGCtx *ctx, Expr *e) {
    if(!e) return irval_void();
    switch(e->kind){
        case EXPR_INT_LIT:   return irval_imm((i64)e->int_val, e->resolved_type?e->resolved_type->kind:TY_I64);
        case EXPR_FLOAT_LIT: return irval_fimm(e->flt_val, e->resolved_type?e->resolved_type->kind:TY_F64);
        case EXPR_BOOL_LIT:  return irval_imm(e->bool_val?1:0, TY_BOOL);
        case EXPR_NULL_LIT:  return irval_imm(0, TY_POINTER);
        case EXPR_CHAR_LIT:  return irval_imm((i64)e->int_val, TY_I8);
        case EXPR_STRING_LIT: {
            char *buf=arena_alloc(ctx->arena, e->str_val.len+1, 1);
            memcpy(buf,e->str_val.ptr,e->str_val.len); buf[e->str_val.len]='\0';
            return irval_global((StrView){.ptr=buf,.len=e->str_val.len}, TY_POINTER);
        }
        case EXPR_IDENT: {
            LocalVar *lv=cg_find_local(ctx,e->str_val);
            if(lv) return ir_emit_load(ctx->fn,ctx->cur_block,lv->ptr,irval_imm(0,TY_I64),lv->type);
            return irval_global(e->str_val, e->resolved_type?e->resolved_type->kind:TY_I64);
        }
        case EXPR_UNARY: {
            TypeKind rty=cg_typekind(e->resolved_type);
            if(e->unary.op==UNOP_PRE_INC||e->unary.op==UNOP_PRE_DEC){
                Expr *in=e->unary.operand; LocalVar *lv=(in->kind==EXPR_IDENT)?cg_find_local(ctx,in->str_val):NULL;
                IRVal loaded=cg_expr(ctx,in); IROp op=(e->unary.op==UNOP_PRE_INC)?IOP_ADD:IOP_SUB;
                IRVal res=ir_emit(ctx->fn,ctx->cur_block,op,loaded,irval_imm(1,rty),rty);
                if(lv) ir_emit_store(ctx->fn,ctx->cur_block,lv->ptr,irval_imm(0,TY_I64),res);
                return res;
            }
            if(e->unary.op==UNOP_POST_INC||e->unary.op==UNOP_POST_DEC){
                Expr *in=e->unary.operand; LocalVar *lv=(in->kind==EXPR_IDENT)?cg_find_local(ctx,in->str_val):NULL;
                IRVal old=cg_expr(ctx,in); IROp op=(e->unary.op==UNOP_POST_INC)?IOP_ADD:IOP_SUB;
                IRVal res=ir_emit(ctx->fn,ctx->cur_block,op,old,irval_imm(1,rty),rty);
                if(lv) ir_emit_store(ctx->fn,ctx->cur_block,lv->ptr,irval_imm(0,TY_I64),res);
                return old;
            }
            IRVal op=cg_expr(ctx,e->unary.operand);
            switch(e->unary.op){
                case UNOP_NEG:  return ir_emit(ctx->fn,ctx->cur_block,IOP_NEG,op,irval_void(),rty);
                case UNOP_NOT:  return ir_emit(ctx->fn,ctx->cur_block,IOP_NOT,op,irval_void(),TY_BOOL);
                case UNOP_BNOT: return ir_emit(ctx->fn,ctx->cur_block,IOP_BNOT,op,irval_void(),rty);
                default: return op;
            }
        }
        case EXPR_BINARY: {
            if(e->binary.op==BINOP_ASSIGN){
                IRVal rv=cg_expr(ctx,e->binary.rhs); Expr *lhs=e->binary.lhs;
                if(lhs->kind==EXPR_IDENT){LocalVar *lv=cg_find_local(ctx,lhs->str_val);if(lv)ir_emit_store(ctx->fn,ctx->cur_block,lv->ptr,irval_imm(0,TY_I64),rv);}
                else if(lhs->kind==EXPR_DEREF){IRVal p=cg_expr(ctx,lhs->deref.operand);ir_emit_store(ctx->fn,ctx->cur_block,p,irval_imm(0,TY_I64),rv);}
                return rv;
            }
            static const struct{BinOpKind c;IROp i;}cm[]={{BINOP_ADD_ASSIGN,IOP_ADD},{BINOP_SUB_ASSIGN,IOP_SUB},{BINOP_MUL_ASSIGN,IOP_MUL},{BINOP_DIV_ASSIGN,IOP_DIV},{BINOP_MOD_ASSIGN,IOP_MOD},{BINOP_AND_ASSIGN,IOP_AND},{BINOP_OR_ASSIGN,IOP_OR},{BINOP_XOR_ASSIGN,IOP_XOR},{BINOP_SHL_ASSIGN,IOP_SHL},{BINOP_SHR_ASSIGN,IOP_SHR}};
            for(u32 ci=0;ci<ARRAY_LEN(cm);ci++){if(e->binary.op==cm[ci].c){IRVal lv=cg_expr(ctx,e->binary.lhs),rv=cg_expr(ctx,e->binary.rhs);TypeKind ty=cg_typekind(e->resolved_type);IRVal res=ir_emit(ctx->fn,ctx->cur_block,cm[ci].i,lv,rv,ty);if(e->binary.lhs->kind==EXPR_IDENT){LocalVar *l=cg_find_local(ctx,e->binary.lhs->str_val);if(l)ir_emit_store(ctx->fn,ctx->cur_block,l->ptr,irval_imm(0,TY_I64),res);}return res;}}
            IRVal lhs=cg_expr(ctx,e->binary.lhs),rhs=cg_expr(ctx,e->binary.rhs);
            TypeKind ty=cg_typekind(e->resolved_type);
            static const struct{BinOpKind b;IROp i;}bm[]={{BINOP_ADD,IOP_ADD},{BINOP_SUB,IOP_SUB},{BINOP_MUL,IOP_MUL},{BINOP_DIV,IOP_DIV},{BINOP_MOD,IOP_MOD},{BINOP_AND,IOP_AND},{BINOP_OR,IOP_OR},{BINOP_XOR,IOP_XOR},{BINOP_SHL,IOP_SHL},{BINOP_SHR,IOP_SHR},{BINOP_LAND,IOP_AND},{BINOP_LOR,IOP_OR},{BINOP_EQ,IOP_CMP_EQ},{BINOP_NEQ,IOP_CMP_NE},{BINOP_LT,IOP_CMP_LT},{BINOP_GT,IOP_CMP_GT},{BINOP_LE,IOP_CMP_LE},{BINOP_GE,IOP_CMP_GE}};
            for(u32 i=0;i<ARRAY_LEN(bm);i++) if(e->binary.op==bm[i].b) return ir_emit(ctx->fn,ctx->cur_block,bm[i].i,lhs,rhs,ty);
            return lhs;
        }
        case EXPR_CALL: {
            IRVal cv=(e->call.callee->kind==EXPR_IDENT)?irval_func(e->call.callee->str_val):cg_expr(ctx,e->call.callee);
            IRVal *args=NULL;
            if(e->call.arg_count>0){args=arena_alloc(ctx->arena,sizeof(IRVal)*e->call.arg_count,alignof(IRVal));for(u32 i=0;i<e->call.arg_count;i++)args[i]=cg_expr(ctx,e->call.args[i]);}
            return ir_emit_call(ctx->fn,ctx->cur_block,cv,args,e->call.arg_count,e->resolved_type?e->resolved_type->kind:TY_VOID);
        }
        case EXPR_INDEX: {
            IRVal base=cg_expr(ctx,e->subscript.base),idx=cg_expr(ctx,e->subscript.index);
            TypeKind ety=cg_typekind(e->resolved_type); u32 sz=MAX(ty_primitive_size(ety),1);
            IRVal off=ir_emit(ctx->fn,ctx->cur_block,IOP_MUL,idx,irval_imm(sz,TY_I64),TY_I64);
            IRVal addr=ir_emit(ctx->fn,ctx->cur_block,IOP_ADD,base,off,TY_POINTER);
            return ir_emit_load(ctx->fn,ctx->cur_block,addr,irval_imm(0,TY_I64),ety);
        }
        case EXPR_DEREF: { IRVal p=cg_expr(ctx,e->deref.operand); return ir_emit_load(ctx->fn,ctx->cur_block,p,irval_imm(0,TY_I64),cg_typekind(e->resolved_type)); }
        case EXPR_ADDR_OF: { Expr *in=e->addr_of.operand; if(in->kind==EXPR_IDENT){LocalVar *lv=cg_find_local(ctx,in->str_val);if(lv)return lv->ptr;} return cg_expr(ctx,in); }
        case EXPR_CAST: {
            IRVal val=cg_expr(ctx,e->cast.operand);
            TypeKind ft=cg_typekind(e->cast.operand->resolved_type),tt=cg_typekind(e->cast.to);
            if(ft==tt) return val;
            if(ty_is_float(ft)&&ty_is_integer(tt)) return ir_emit(ctx->fn,ctx->cur_block,IOP_FTOI,val,irval_void(),tt);
            if(ty_is_integer(ft)&&ty_is_float(tt)) return ir_emit(ctx->fn,ctx->cur_block,IOP_ITOF,val,irval_void(),tt);
            u32 fs=ty_primitive_size(ft),ts=ty_primitive_size(tt);
            if(ts>fs) return ir_emit(ctx->fn,ctx->cur_block,ty_is_signed(ft)?IOP_SEXT:IOP_ZEXT,val,irval_void(),tt);
            if(ts<fs) return ir_emit(ctx->fn,ctx->cur_block,IOP_TRUNC,val,irval_void(),tt);
            return ir_emit(ctx->fn,ctx->cur_block,IOP_BITCAST,val,irval_void(),tt);
        }
        case EXPR_SIZEOF: return irval_imm(ty_primitive_size(cg_typekind(e->sizeof_expr.type)),TY_USIZE);
        case EXPR_ALIGNOF: return irval_imm(ty_primitive_size(cg_typekind(e->sizeof_expr.type)),TY_USIZE);
        case EXPR_FIELD: return ir_emit_load(ctx->fn,ctx->cur_block,cg_expr(ctx,e->field.base),irval_imm(0,TY_I64),cg_typekind(e->resolved_type));
        case EXPR_STRUCT_INIT: for(u32 i=0;i<e->struct_init.field_count;i++) cg_expr(ctx,e->struct_init.fields[i].value); return irval_imm(0,TY_POINTER);
        default: return irval_imm(0,TY_I64);
    }
}

static void cg_emit_defers(CGCtx *ctx) {
    for(i32 i=(i32)ctx->defer_count-1;i>=0;i--) cg_stmt(ctx,ctx->defers[i]);
}

static void cg_stmt(CGCtx *ctx, Stmt *s) {
    if(!s) return;
    switch(s->kind){
        case STMT_BLOCK: {
            u32 sl=ctx->local_count,sd=ctx->defer_count;
            for(u32 i=0;i<s->block.count;i++) cg_stmt(ctx,s->block.stmts[i]);
            for(i32 i=(i32)ctx->defer_count-1;i>=(i32)sd;i--) cg_stmt(ctx,ctx->defers[i]);
            ctx->local_count=sl; ctx->defer_count=sd; break;
        }
        case STMT_LET: {
            TypeKind ty=s->let.type?s->let.type->kind:TY_I64;
            u32 sz=MAX(ty_primitive_size(ty),8); if(!sz)sz=8;
            IRVal ptr=ir_emit_alloca(ctx->fn,ctx->cur_block,ty,sz);
            if(ctx->local_count<MAX_LOCALS) ctx->locals[ctx->local_count++]=(LocalVar){.name=s->let.name,.ptr=ptr,.type=ty};
            if(s->let.init){IRVal v=cg_expr(ctx,s->let.init);ir_emit_store(ctx->fn,ctx->cur_block,ptr,irval_imm(0,TY_I64),v);}
            break;
        }
        case STMT_EXPR: cg_expr(ctx,s->expr_stmt.expr); break;
        case STMT_RETURN:
            cg_emit_defers(ctx);
            if(s->ret.value){IRVal v=cg_expr(ctx,s->ret.value);ir_emit_ret(ctx->fn,ctx->cur_block,v);}
            else ir_emit_ret(ctx->fn,ctx->cur_block,irval_void());
            ctx->cur_block=cg_new_block(ctx,"dead"); break;
        case STMT_IF: {
            u32 tl=ir_new_label(ctx->fn),el=ir_new_label(ctx->fn),endl=ir_new_label(ctx->fn);
            IRVal cond=cg_expr(ctx,s->if_stmt.cond);
            ir_emit_branch(ctx->fn,ctx->cur_block,cond,tl,el);
            IRBlock *tb=cg_new_block(ctx,"if.then"); ir_emit(ctx->fn,tb,IOP_LABEL,irval_label(tl),irval_void(),TY_VOID);
            cg_switch_block(ctx,tb); cg_stmt(ctx,s->if_stmt.then_body); ir_emit_jmp(ctx->fn,ctx->cur_block,endl);
            IRBlock *eb=cg_new_block(ctx,"if.else"); ir_emit(ctx->fn,eb,IOP_LABEL,irval_label(el),irval_void(),TY_VOID);
            cg_switch_block(ctx,eb); if(s->if_stmt.else_body) cg_stmt(ctx,s->if_stmt.else_body); ir_emit_jmp(ctx->fn,ctx->cur_block,endl);
            IRBlock *endb=cg_new_block(ctx,"if.end"); ir_emit(ctx->fn,endb,IOP_LABEL,irval_label(endl),irval_void(),TY_VOID);
            cg_switch_block(ctx,endb); break;
        }
        case STMT_WHILE: {
            u32 cl=ir_new_label(ctx->fn),bl=ir_new_label(ctx->fn),el=ir_new_label(ctx->fn);
            if(ctx->loop_depth<32){ctx->break_labels[ctx->loop_depth]=el;ctx->continue_labels[ctx->loop_depth]=cl;ctx->loop_depth++;}
            ir_emit_jmp(ctx->fn,ctx->cur_block,cl);
            IRBlock *cb=cg_new_block(ctx,"while.cond"); ir_emit(ctx->fn,cb,IOP_LABEL,irval_label(cl),irval_void(),TY_VOID);
            cg_switch_block(ctx,cb); IRVal cond=cg_expr(ctx,s->while_stmt.cond); ir_emit_branch(ctx->fn,ctx->cur_block,cond,bl,el);
            IRBlock *bb=cg_new_block(ctx,"while.body"); ir_emit(ctx->fn,bb,IOP_LABEL,irval_label(bl),irval_void(),TY_VOID);
            cg_switch_block(ctx,bb); cg_stmt(ctx,s->while_stmt.body); ir_emit_jmp(ctx->fn,ctx->cur_block,cl);
            IRBlock *eb=cg_new_block(ctx,"while.end"); ir_emit(ctx->fn,eb,IOP_LABEL,irval_label(el),irval_void(),TY_VOID);
            cg_switch_block(ctx,eb); if(ctx->loop_depth>0)ctx->loop_depth--; break;
        }
        case STMT_FOR: {
            u32 cl=ir_new_label(ctx->fn),bl=ir_new_label(ctx->fn),pl=ir_new_label(ctx->fn),el=ir_new_label(ctx->fn);
            if(ctx->loop_depth<32){ctx->break_labels[ctx->loop_depth]=el;ctx->continue_labels[ctx->loop_depth]=pl;ctx->loop_depth++;}
            if(s->for_stmt.init) cg_stmt(ctx,s->for_stmt.init);
            ir_emit_jmp(ctx->fn,ctx->cur_block,cl);
            IRBlock *cb=cg_new_block(ctx,"for.cond"); ir_emit(ctx->fn,cb,IOP_LABEL,irval_label(cl),irval_void(),TY_VOID); cg_switch_block(ctx,cb);
            if(s->for_stmt.cond){IRVal c=cg_expr(ctx,s->for_stmt.cond);ir_emit_branch(ctx->fn,ctx->cur_block,c,bl,el);}else ir_emit_jmp(ctx->fn,ctx->cur_block,bl);
            IRBlock *bb=cg_new_block(ctx,"for.body"); ir_emit(ctx->fn,bb,IOP_LABEL,irval_label(bl),irval_void(),TY_VOID); cg_switch_block(ctx,bb);
            cg_stmt(ctx,s->for_stmt.body); ir_emit_jmp(ctx->fn,ctx->cur_block,pl);
            IRBlock *pb=cg_new_block(ctx,"for.post"); ir_emit(ctx->fn,pb,IOP_LABEL,irval_label(pl),irval_void(),TY_VOID); cg_switch_block(ctx,pb);
            if(s->for_stmt.post) cg_stmt(ctx,s->for_stmt.post); ir_emit_jmp(ctx->fn,ctx->cur_block,cl);
            IRBlock *eb=cg_new_block(ctx,"for.end"); ir_emit(ctx->fn,eb,IOP_LABEL,irval_label(el),irval_void(),TY_VOID); cg_switch_block(ctx,eb);
            if(ctx->loop_depth>0)ctx->loop_depth--; break;
        }
        case STMT_BREAK:
            if(ctx->loop_depth>0){cg_emit_defers(ctx);ir_emit_jmp(ctx->fn,ctx->cur_block,ctx->break_labels[ctx->loop_depth-1]);ctx->cur_block=cg_new_block(ctx,"after.break");}break;
        case STMT_CONTINUE:
            if(ctx->loop_depth>0){ir_emit_jmp(ctx->fn,ctx->cur_block,ctx->continue_labels[ctx->loop_depth-1]);ctx->cur_block=cg_new_block(ctx,"after.continue");}break;
        case STMT_DEFER:
            if(ctx->defer_count<MAX_DEFERS) ctx->defers[ctx->defer_count++]=s->defer_stmt.deferred; break;
        default: break;
    }
}

static void cg_func(CGCtx *ctx, Decl *d) {
    StrView *pn=NULL; TypeKind *pt=NULL;
    if(d->func.param_count>0){
        pn=arena_alloc(ctx->arena,sizeof(StrView)*d->func.param_count,alignof(StrView));
        pt=arena_alloc(ctx->arena,sizeof(TypeKind)*d->func.param_count,alignof(TypeKind));
        for(u32 i=0;i<d->func.param_count;i++){pn[i]=d->func.params[i].name;pt[i]=cg_typekind(d->func.params[i].type);}
    }
    IRFunc *fn=ir_func_new(ctx->module,d->name,pn,pt,d->func.param_count,d->func.ret_type,d->func.variadic);
    ctx->fn=fn; ctx->local_count=0; ctx->defer_count=0; ctx->loop_depth=0;
    IRBlock *entry=cg_new_block(ctx,"entry"); cg_switch_block(ctx,entry);
    for(u32 i=0;i<d->func.param_count;i++){
        TypeKind ty=pt[i]; u32 sz=MAX(ty_primitive_size(ty),8);
        IRVal ptr=ir_emit_alloca(fn,entry,ty,sz);
        ir_emit_store(fn,entry,ptr,irval_imm(0,TY_I64),irval_temp(i,ty));
        if(ctx->local_count<MAX_LOCALS) ctx->locals[ctx->local_count++]=(LocalVar){.name=pn[i],.ptr=ptr,.type=ty};
    }
    if(d->func.body) cg_stmt(ctx,d->func.body);
    if(ctx->cur_block->instr_count==0||(ctx->cur_block->instrs[ctx->cur_block->instr_count-1].op!=IOP_RET&&ctx->cur_block->instrs[ctx->cur_block->instr_count-1].op!=IOP_RET_VOID))
        {cg_emit_defers(ctx);ir_emit_ret(fn,ctx->cur_block,irval_void());}
}

IRModule *codegen_module(AstModule *ast, Arena *arena) {
    IRModule *m=ir_module_new(arena);
    CGCtx ctx={.module=m,.arena=arena};
    for(u32 i=0;i<ast->decl_count;i++){
        Decl *d=ast->decls[i]; if(!d) continue;
        if(d->kind==DECL_FUNC&&!d->is_extern&&d->func.body) cg_func(&ctx,d);
    }
    return m;
}
