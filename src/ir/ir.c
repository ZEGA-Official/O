// ============================================================
//  O Language Compiler -- o_ir.c
//  Three-address IR builder implementation
//  Z-TEAM | C23
// ============================================================
#include "ir/ir.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

IRModule *ir_module_new(Arena *arena) {
    IRModule *m  = ARENA_ALLOC(arena, IRModule);
    m->arena      = arena;
    m->func_count = 0;
    m->func_cap   = 16;
    m->funcs      = arena_alloc(arena, sizeof(IRFunc*) * m->func_cap, alignof(IRFunc*));
    return m;
}

IRFunc *ir_func_new(IRModule *m, StrView name,
                    StrView *param_names, TypeKind *param_types,
                    u32 param_count, TypeNode *ret_type, bool variadic) {
    if (m->func_count == m->func_cap) {
        u32 nc = m->func_cap * 2;
        IRFunc **nf = arena_alloc(m->arena, sizeof(IRFunc*)*nc, alignof(IRFunc*));
        memcpy(nf, m->funcs, sizeof(IRFunc*)*m->func_count);
        m->funcs = nf; m->func_cap = nc;
    }
    Arena *fa = arena_new(MB(2));
    IRFunc *fn     = ARENA_ALLOC(fa, IRFunc);
    fn->name        = name;
    fn->ret_type    = ret_type;
    fn->variadic    = variadic;
    fn->param_count = param_count;
    fn->next_vreg   = param_count;
    fn->next_label  = 0;
    fn->block_count = 0;
    fn->block_cap   = 16;
    fn->arena       = fa;
    fn->blocks      = arena_alloc(fa, sizeof(IRBlock*)*fn->block_cap, alignof(IRBlock*));
    if (param_count > 0) {
        fn->param_names = arena_alloc(fa, sizeof(StrView)*param_count, alignof(StrView));
        fn->param_types = arena_alloc(fa, sizeof(TypeKind)*param_count, alignof(TypeKind));
        memcpy(fn->param_names, param_names, sizeof(StrView)*param_count);
        memcpy(fn->param_types, param_types, sizeof(TypeKind)*param_count);
    } else { fn->param_names = NULL; fn->param_types = NULL; }
    m->funcs[m->func_count++] = fn;
    return fn;
}

IRBlock *ir_block_new(IRFunc *fn, StrView name) {
    if (fn->block_count == fn->block_cap) {
        u32 nc = fn->block_cap * 2;
        IRBlock **nb = arena_alloc(fn->arena, sizeof(IRBlock*)*nc, alignof(IRBlock*));
        memcpy(nb, fn->blocks, sizeof(IRBlock*)*fn->block_count);
        fn->blocks = nb; fn->block_cap = nc;
    }
    IRBlock *b   = ARENA_ALLOC(fn->arena, IRBlock);
    b->id         = fn->block_count;
    b->name       = name;
    b->instr_count = 0;
    b->instr_cap   = 32;
    b->instrs      = arena_alloc(fn->arena, sizeof(IRInstr)*b->instr_cap, alignof(IRInstr));
    b->succ_count  = 0; b->pred_count = 0;
    b->preds = NULL; b->live_in = NULL; b->live_out = NULL;
    b->succ[0] = NULL; b->succ[1] = NULL;
    fn->blocks[fn->block_count++] = b;
    return b;
}

void ir_block_seal(IRFunc *fn, IRBlock *b) { UNUSED(fn); UNUSED(b); }

static IRInstr *block_alloc_instr(IRFunc *fn, IRBlock *b) {
    if (b->instr_count == b->instr_cap) {
        u32 nc = b->instr_cap * 2;
        IRInstr *ni = arena_alloc(fn->arena, sizeof(IRInstr)*nc, alignof(IRInstr));
        memcpy(ni, b->instrs, sizeof(IRInstr)*b->instr_count);
        b->instrs = ni; b->instr_cap = nc;
    }
    IRInstr *ins = &b->instrs[b->instr_count++];
    memset(ins, 0, sizeof(*ins));
    return ins;
}

IRVal ir_emit(IRFunc *fn, IRBlock *b, IROp op,
              IRVal src1, IRVal src2, TypeKind result_type) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = op; ins->src1 = src1; ins->src2 = src2;
    ins->result_type = result_type; ins->arg_count = 0; ins->args = NULL;
    bool has_result = (result_type != TY_VOID)
        && op != IOP_STORE && op != IOP_JMP && op != IOP_JZ && op != IOP_JNZ
        && op != IOP_RET && op != IOP_RET_VOID && op != IOP_LABEL
        && op != IOP_NOP && op != IOP_MEMCPY && op != IOP_MEMSET;
    if (has_result) { u32 vr = ir_new_vreg(fn); ins->dst = irval_temp(vr, result_type); return ins->dst; }
    ins->dst = irval_void(); return irval_void();
}

IRVal ir_emit_call(IRFunc *fn, IRBlock *b, IRVal func_val,
                   IRVal *args, u32 arg_count, TypeKind ret_type) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_CALL; ins->src1 = func_val; ins->src2 = irval_void();
    ins->result_type = ret_type; ins->arg_count = arg_count;
    if (arg_count > 0) {
        ins->args = arena_alloc(fn->arena, sizeof(IRVal)*arg_count, alignof(IRVal));
        memcpy(ins->args, args, sizeof(IRVal)*arg_count);
    } else ins->args = NULL;
    if (ret_type != TY_VOID) { u32 vr = ir_new_vreg(fn); ins->dst = irval_temp(vr, ret_type); return ins->dst; }
    ins->dst = irval_void(); return irval_void();
}

IRVal ir_emit_alloca(IRFunc *fn, IRBlock *b, TypeKind ty, u32 size) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_ALLOCA; ins->src1 = irval_imm(size, TY_U32);
    ins->src2 = irval_void(); ins->result_type = TY_POINTER;
    ins->arg_count = 0; ins->args = NULL;
    u32 vr = ir_new_vreg(fn); ins->dst = irval_temp(vr, TY_POINTER);
    UNUSED(ty); return ins->dst;
}

void ir_emit_store(IRFunc *fn, IRBlock *b, IRVal ptr, IRVal offset, IRVal value) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_STORE; ins->dst = ptr; ins->src1 = offset; ins->src2 = value;
    ins->result_type = TY_VOID; ins->arg_count = 0; ins->args = NULL; UNUSED(fn);
}

IRVal ir_emit_load(IRFunc *fn, IRBlock *b, IRVal ptr, IRVal offset, TypeKind ty) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_LOAD; ins->src1 = ptr; ins->src2 = offset;
    ins->result_type = ty; ins->arg_count = 0; ins->args = NULL;
    u32 vr = ir_new_vreg(fn); ins->dst = irval_temp(vr, ty); return ins->dst;
}

void ir_emit_jmp(IRFunc *fn, IRBlock *b, u32 label_id) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_JMP; ins->dst = irval_void();
    ins->src1 = irval_label(label_id); ins->src2 = irval_void();
    ins->result_type = TY_VOID; ins->arg_count = 0; ins->args = NULL; UNUSED(fn);
}

void ir_emit_branch(IRFunc *fn, IRBlock *b, IRVal cond, u32 true_label, u32 false_label) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = IOP_JNZ; ins->dst = irval_void(); ins->src1 = cond;
    ins->src2 = irval_label(true_label); ins->result_type = TY_VOID;
    ins->arg_count = 0; ins->args = NULL;
    ir_emit_jmp(fn, b, false_label);
}

void ir_emit_ret(IRFunc *fn, IRBlock *b, IRVal val) {
    IRInstr *ins = block_alloc_instr(fn, b);
    ins->op = (val.kind == IRVAL_VOID) ? IOP_RET_VOID : IOP_RET;
    ins->src1 = val; ins->dst = irval_void(); ins->src2 = irval_void();
    ins->result_type = TY_VOID; ins->arg_count = 0; ins->args = NULL; UNUSED(fn);
}

const char *irop_name(IROp op) {
    static const char *n[IOP_COUNT] = {
        [IOP_ADD]="add",[IOP_SUB]="sub",[IOP_MUL]="mul",[IOP_DIV]="div",[IOP_MOD]="mod",
        [IOP_AND]="and",[IOP_OR]="or",[IOP_XOR]="xor",[IOP_SHL]="shl",[IOP_SHR]="shr",[IOP_SAR]="sar",
        [IOP_NEG]="neg",[IOP_NOT]="not",[IOP_BNOT]="bnot",
        [IOP_FADD]="fadd",[IOP_FSUB]="fsub",[IOP_FMUL]="fmul",[IOP_FDIV]="fdiv",[IOP_FNEG]="fneg",
        [IOP_CMP_EQ]="eq",[IOP_CMP_NE]="ne",[IOP_CMP_LT]="lt",[IOP_CMP_LE]="le",
        [IOP_CMP_GT]="gt",[IOP_CMP_GE]="ge",
        [IOP_MOV]="mov",[IOP_LOAD]="load",[IOP_STORE]="store",[IOP_LEA]="lea",
        [IOP_SEXT]="sext",[IOP_ZEXT]="zext",[IOP_TRUNC]="trunc",
        [IOP_ITOF]="itof",[IOP_FTOI]="ftoi",[IOP_BITCAST]="bitcast",
        [IOP_LABEL]="label",[IOP_JMP]="jmp",[IOP_JZ]="jz",[IOP_JNZ]="jnz",
        [IOP_CALL]="call",[IOP_RET]="ret",[IOP_RET_VOID]="ret.void",
        [IOP_ALLOCA]="alloca",[IOP_MEMCPY]="memcpy",[IOP_MEMSET]="memset",[IOP_NOP]="nop",
    };
    return (op>=0&&op<IOP_COUNT&&n[op]) ? n[op] : "???";
}

static const char *tkn(TypeKind k) {
    switch(k){case TY_VOID:return"void";case TY_BOOL:return"bool";
    case TY_I8:return"i8";case TY_I16:return"i16";case TY_I32:return"i32";case TY_I64:return"i64";
    case TY_U8:return"u8";case TY_U16:return"u16";case TY_U32:return"u32";case TY_U64:return"u64";
    case TY_F32:return"f32";case TY_F64:return"f64";
    case TY_POINTER:return"*";default:return"?";}
}

static void dump_val(const IRVal *v, FILE *out) {
    switch(v->kind){
        case IRVAL_VOID: fprintf(out,"void"); break;
        case IRVAL_TEMP: fprintf(out,"%%%u:%s",v->vreg,tkn(v->type)); break;
        case IRVAL_CONST_I: fprintf(out,"%" PRId64 ":%s",v->ival,tkn(v->type)); break;
        case IRVAL_CONST_F: fprintf(out,"%g:%s",v->fval,tkn(v->type)); break;
        case IRVAL_GLOBAL: fprintf(out,"@%.*s",(int)v->name.len,v->name.ptr); break;
        case IRVAL_FUNC: fprintf(out,"fn:%.*s",(int)v->name.len,v->name.ptr); break;
        case IRVAL_LABEL: fprintf(out,"L%u",v->label_id); break;
    }
}

void ir_func_dump(const IRFunc *fn, FILE *out) {
    fprintf(out,"\033[38;2;88;240;27mfn\033[0m %.*s(",(int)fn->name.len,fn->name.ptr);
    for(u32 i=0;i<fn->param_count;i++){if(i)fprintf(out,", ");fprintf(out,"%%%u:%s",i,tkn(fn->param_types[i]));}
    fprintf(out,") -> %s {\n",fn->ret_type?tkn(fn->ret_type->kind):"void");
    for(u32 bi=0;bi<fn->block_count;bi++){
        const IRBlock *b=fn->blocks[bi];
        fprintf(out,"  \033[33mB%u\033[0m",b->id);
        if(b->name.len) fprintf(out,"(%.*s)",(int)b->name.len,b->name.ptr);
        fprintf(out,":\n");
        for(u32 ii=0;ii<b->instr_count;ii++){
            const IRInstr *ins=&b->instrs[ii];
            fprintf(out,"    ");
            if(ins->dst.kind!=IRVAL_VOID){dump_val(&ins->dst,out);fprintf(out," = ");}
            fprintf(out,"%s",irop_name(ins->op));
            if(ins->op==IOP_CALL){fprintf(out," ");dump_val(&ins->src1,out);fprintf(out,"(");
                for(u32 a=0;a<ins->arg_count;a++){if(a)fprintf(out,", ");dump_val(&ins->args[a],out);}
                fprintf(out,")");}
            else{if(ins->src1.kind!=IRVAL_VOID){fprintf(out," ");dump_val(&ins->src1,out);}
                 if(ins->src2.kind!=IRVAL_VOID){fprintf(out,", ");dump_val(&ins->src2,out);}}
            fprintf(out,"\n");
        }
    }
    fprintf(out,"}\n\n");
}

void ir_module_dump(const IRModule *m, FILE *out) {
    fprintf(out,"; --- O IR Module ---\n\n");
    for(u32 i=0;i<m->func_count;i++) ir_func_dump(m->funcs[i],out);
}
