// ============================================================
//  O Language Compiler -- o_elf_exec.c
//  ELF64 executable and .so emitter
//  Z-TEAM | C23
// ============================================================
#include "lib/elf/exec.h"
#include "lib/elf/aot.h"
#include "backend/x64.h"
#include "jit/jit.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#include <dlfcn.h>
#endif
#include <sys/stat.h>

#ifndef ELFCLASS64
#  define ELFCLASS64  2
#  define ELFDATA2LSB 1
#endif
#ifndef ET_EXEC
#  define ET_EXEC 2
#  define ET_DYN  3
#endif
#ifndef EM_X86_64
#  define EM_X86_64 62
#endif
#ifndef EV_CURRENT
#  define EV_CURRENT 1
#endif
#ifndef PT_LOAD
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#endif
#ifndef SHT_NULL
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_STRTAB   3
#define SHT_DYNSYM   11
#endif
#ifndef SHF_ALLOC
#define SHF_ALLOC     0x02
#define SHF_EXECINSTR 0x04
#endif
#define DT_NULL    0
#define DT_NEEDED  1
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_STRSZ   10
#define DT_SYMENT  11
#define DT_PLTGOT  3
#define DT_FLAGS   30

typedef struct { u8 e_ident[16]; u16 e_type,e_machine; u32 e_version;
    u64 e_entry,e_phoff,e_shoff; u32 e_flags;
    u16 e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx; } EE_Ehdr;
typedef struct { u32 p_type,p_flags; u64 p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_align; } EE_Phdr;
typedef struct { u64 sh_name,sh_type,sh_flags,sh_addr,sh_offset,sh_size,sh_link,sh_info,sh_addralign,sh_entsize; } EE_Shdr;
typedef struct { u32 st_name; u8 st_info,st_other; u16 st_shndx; u64 st_value,st_size; } EE_Sym;
typedef struct { i64 d_tag; u64 d_val; } EE_Dyn;

static void ee_wbuf(int fd, const void *p, usize n) { (void)write(fd, p, n); }
static void ee_wpad(int fd, usize n) { u8 z=0; for(usize i=0;i<n;i++)(void)write(fd,&z,1); }
static usize ee_pos(int fd) { return (usize)lseek(fd, 0, SEEK_CUR); }
static void ee_align(int fd, usize a) { usize cur=ee_pos(fd),rem=cur%a; if(rem)ee_wpad(fd,a-rem); }

ElfExecCtx *elf_exec_new(Arena *arena, ElfExecMode mode) {
    ElfExecCtx *ctx = ARENA_ALLOC_ZERO(arena, ElfExecCtx);
    ctx->arena=arena; ctx->mode=mode;
    ctx->interp="/lib64/ld-linux-x86-64.so.2";
    ctx->text_cap=MB(4); ctx->data_cap=MB(1);
    ctx->text_buf=arena_alloc(arena,ctx->text_cap,16);
    ctx->data_buf=arena_alloc(arena,ctx->data_cap,16);
    ctx->load_vaddr=(mode==ELF_EXEC_MODE_STATIC)?0x400000ULL:0;
    return ctx;
}

void elf_exec_add_needed(ElfExecCtx *ctx, StrView lib) {
    if(ctx->needed_count<32) ctx->needed_libs[ctx->needed_count++]=lib;
}

u32 elf_exec_declare_extern(ElfExecCtx *ctx, StrView sym) {
    for(u32 i=0;i<ctx->extern_count;i++) if(sv_eq(ctx->extern_syms[i],sym)) return i;
    if(ctx->extern_count>=256) return 0;
    u32 idx=ctx->extern_count++;
    ctx->extern_syms[idx]=sym; ctx->extern_got_off[idx]=idx*8;
    return idx;
}

void elf_exec_add_export(ElfExecCtx *ctx, StrView name, u32 off) {
    if(ctx->export_count<256){
        ctx->export_names[ctx->export_count]=name;
        ctx->export_text_off[ctx->export_count]=off;
        ctx->export_count++;
    }
}

ElfExecResult elf_exec_compile(ElfExecCtx *ctx, IRModule *module) {
    Arena *aa=arena_new(MB(16));
    AOTContext *aot=aot_context_new(aa);
    AOTOptions opts={.optimize=true};
    OResult r=aot_compile_module(aot,module,&opts);
    if(r.status!=O_OK) return (ElfExecResult){.had_error=true,.msg=r.msg};
    if(ctx->text_size+aot->text.len>ctx->text_cap){
        usize nc=MAX(ctx->text_cap*2,ctx->text_size+aot->text.len);
        u8 *nb=arena_alloc(ctx->arena,nc,16);
        memcpy(nb,ctx->text_buf,ctx->text_size);
        ctx->text_buf=nb; ctx->text_cap=nc;
    }
    memcpy(ctx->text_buf+ctx->text_size,aot->text.buf,aot->text.len);
    for(u32 i=0;i<aot->func_count;i++)
        elf_exec_add_export(ctx,aot->func_names[i],(u32)aot->func_offsets[i]);
    for(u32 i=0;i<aot->extern_count;i++)
        elf_exec_declare_extern(ctx,aot->extern_names[i]);
    if(ctx->mode!=ELF_EXEC_MODE_STATIC){
        elf_exec_add_needed(ctx,sv_from_cstr("libc.so.6"));
    }
    ctx->text_size=aot->text.len; ctx->entry_text_off=0;
    return (ElfExecResult){0};
}

ElfExecResult elf_exec_write(ElfExecCtx *ctx, const char *path) {
    bool is_so  = (ctx->mode==ELF_EXEC_MODE_SO);
    bool is_dyn = (ctx->mode==ELF_EXEC_MODE_DYNAMIC||is_so);
    u64  base   = ctx->load_vaddr;
    const u64 PAGE=0x1000;

    u8 strtab[8192]; u32 strsz=1; strtab[0]=0;
    u8 symstrtab[4096]; u32 symstrsz=1; symstrtab[0]=0;

    #define STRTAB_ADD(s_) ({ const char *_s=(s_); usize _l=strlen(_s)+1; u32 _off=strsz; memcpy(strtab+strsz,_s,_l); strsz+=(u32)_l; _off; })
    #define SYMSTR_ADD(s_) ({ const char *_s=(s_); usize _l=strlen(_s)+1; u32 _off=symstrsz; memcpy(symstrtab+symstrsz,_s,_l); symstrsz+=(u32)_l; _off; })

    u8 dynsym_buf[4096]; u32 dynsym_sz=0;
    EE_Sym null_sym={0}; memcpy(dynsym_buf,&null_sym,sizeof(null_sym)); dynsym_sz+=sizeof(null_sym);

    usize text_filesz=ALIGN_UP(ctx->text_size,PAGE);
    usize interp_len=is_dyn?strlen(ctx->interp)+1:0;
    u64 va_text=base+PAGE*2, va_got=base+PAGE*2+(u64)text_filesz;
    u64 va_dyn=va_got+PAGE, va_dynstr=va_dyn+PAGE, va_dynsym=va_dynstr+PAGE;
    UNUSED(va_dynsym);

    for(u32 i=0;i<ctx->export_count;i++){
        EE_Sym s={0}; char nb[256]; usize nl=MIN(ctx->export_names[i].len,255);
        memcpy(nb,ctx->export_names[i].ptr,nl); nb[nl]=0;
        s.st_name=SYMSTR_ADD(nb); s.st_info=(1<<4)|2; s.st_shndx=1;
        s.st_value=va_text+ctx->export_text_off[i];
        memcpy(dynsym_buf+dynsym_sz,&s,sizeof(s)); dynsym_sz+=sizeof(s);
    }

    u8 dyn_buf[2048]; u32 dyn_sz=0;
    #define DYN_ADD(tag_,val_) do{ EE_Dyn d_={.d_tag=(tag_),.d_val=(u64)(val_)}; memcpy(dyn_buf+dyn_sz,&d_,sizeof(d_)); dyn_sz+=sizeof(d_); }while(0)
    if(is_dyn){
        for(u32 i=0;i<ctx->needed_count;i++){
            char nm[64]; usize l=MIN(ctx->needed_libs[i].len,63);
            memcpy(nm,ctx->needed_libs[i].ptr,l); nm[l]=0;
            DYN_ADD(DT_NEEDED,STRTAB_ADD(nm));
        }
        DYN_ADD(DT_STRTAB,va_dynstr); DYN_ADD(DT_STRSZ,strsz);
        DYN_ADD(DT_PLTGOT,va_got); DYN_ADD(DT_FLAGS,0x8); DYN_ADD(DT_NULL,0);
    }

    usize ph_off=sizeof(EE_Ehdr); u32 ph_count=is_dyn?5:2;
    usize interp_off=ph_off+ph_count*sizeof(EE_Phdr);
    usize text_off=ALIGN_UP(interp_off+interp_len,PAGE);
    usize dyn_off=text_off+text_filesz+PAGE;
    usize dynstr_off=dyn_off+ALIGN_UP(dyn_sz,8);
    usize dynsym_off=dynstr_off+ALIGN_UP(strsz,8);

    u8 shstrtab[256]; u32 shstrsz=0; shstrtab[shstrsz++]=0;
    u32 shn_text=shstrsz; memcpy(shstrtab+shstrsz,".text\0",6); shstrsz+=6;
    u32 shn_shstr=shstrsz; memcpy(shstrtab+shstrsz,".shstrtab\0",10); shstrsz+=10;
    usize shstr_off=dynsym_off+ALIGN_UP(dynsym_sz,8);
    usize sh_off=ALIGN_UP(shstr_off+shstrsz,8);
    u32 sh_count=3;
    UNUSED(shn_text); UNUSED(shn_shstr);

    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0755);
    if(fd<0) return (ElfExecResult){.had_error=true,.msg="cannot open"};

    EE_Ehdr eh={0};
    memcpy(eh.e_ident,"\x7fELF",4);
    eh.e_ident[4]=ELFCLASS64; eh.e_ident[5]=ELFDATA2LSB; eh.e_ident[6]=EV_CURRENT;
    eh.e_type=is_so?ET_DYN:ET_EXEC; eh.e_machine=EM_X86_64; eh.e_version=EV_CURRENT;
    eh.e_entry=va_text+ctx->entry_text_off; eh.e_phoff=(u64)ph_off; eh.e_shoff=(u64)sh_off;
    eh.e_ehsize=sizeof(EE_Ehdr); eh.e_phentsize=sizeof(EE_Phdr); eh.e_phnum=ph_count;
    eh.e_shentsize=sizeof(EE_Shdr); eh.e_shnum=sh_count; eh.e_shstrndx=sh_count-1;
    ee_wbuf(fd,&eh,sizeof(eh));

    if(is_dyn){
        EE_Phdr pp={PT_PHDR,PF_R,(u64)ph_off,base+ph_off,base+ph_off,(u64)(ph_count*sizeof(EE_Phdr)),(u64)(ph_count*sizeof(EE_Phdr)),8};
        ee_wbuf(fd,&pp,sizeof(pp));
        EE_Phdr pi={PT_INTERP,PF_R,(u64)interp_off,base+interp_off,base+interp_off,(u64)interp_len,(u64)interp_len,1};
        ee_wbuf(fd,&pi,sizeof(pi));
    }
    EE_Phdr phl={PT_LOAD,PF_R,0,base,base,(u64)text_off,(u64)text_off,PAGE};
    ee_wbuf(fd,&phl,sizeof(phl));
    EE_Phdr pht={PT_LOAD,PF_R|PF_X,(u64)text_off,va_text,va_text,(u64)text_filesz,(u64)text_filesz,PAGE};
    ee_wbuf(fd,&pht,sizeof(pht));
    if(is_dyn){
        EE_Phdr pd={PT_DYNAMIC,PF_R|PF_W,(u64)dyn_off,va_dyn,va_dyn,(u64)dyn_sz,(u64)dyn_sz,8};
        ee_wbuf(fd,&pd,sizeof(pd));
    }

    ee_wpad(fd,interp_off-ee_pos(fd));
    if(is_dyn) ee_wbuf(fd,ctx->interp,interp_len);
    usize cur=ee_pos(fd); if(cur<text_off) ee_wpad(fd,text_off-cur);
    ee_wbuf(fd,ctx->text_buf,ctx->text_size); ee_align(fd,PAGE);
    ee_wpad(fd,PAGE);
    if(is_dyn){ ee_wbuf(fd,dyn_buf,dyn_sz); ee_align(fd,8);
        ee_wbuf(fd,strtab,strsz); ee_align(fd,8);
        ee_wbuf(fd,dynsym_buf,dynsym_sz); ee_align(fd,8); }
    ee_wbuf(fd,shstrtab,shstrsz); ee_align(fd,8);
    cur=ee_pos(fd); if(cur<sh_off) ee_wpad(fd,sh_off-cur);
    EE_Shdr nsh={0}; ee_wbuf(fd,&nsh,sizeof(nsh));
    EE_Shdr sht={shn_text,SHT_PROGBITS,SHF_ALLOC|SHF_EXECINSTR,va_text,(u64)text_off,ctx->text_size,0,0,16,0};
    ee_wbuf(fd,&sht,sizeof(sht));
    EE_Shdr shs={shn_shstr,SHT_STRTAB,0,0,(u64)shstr_off,shstrsz,0,0,1,0};
    ee_wbuf(fd,&shs,sizeof(shs));
    close(fd); chmod(path,0755);
    return (ElfExecResult){0};
}
