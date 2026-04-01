// ============================================================
//  O Language Compiler — o_pe.h
//  PE32+ (Windows x64) executable and DLL emitter
//  Produces .exe and .dll without any external tools
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"
#include "ir/ir.h"
#include "backend/target.h"

#define PE_MZ_MAGIC       0x5A4D
#define PE_SIGNATURE      0x00004550
#define PE_MACHINE_AMD64  0x8664

#define PE_SCN_CNT_CODE       0x00000020
#define PE_SCN_CNT_INIT_DATA  0x00000040
#define PE_SCN_MEM_EXECUTE    0x20000000
#define PE_SCN_MEM_READ       0x40000000
#define PE_SCN_MEM_WRITE      0x80000000

#define PE_SUBSYSTEM_CONSOLE  3
#define PE_DLLCHAR_DYNAMIC_BASE 0x0040
#define PE_DLLCHAR_NX_COMPAT    0x0100
#define PE_FILE_EXECUTABLE    0x0002
#define PE_FILE_LARGE_ADDRESS 0x0020
#define PE_FILE_DLL           0x2000

#define PE_DIR_EXPORT  0
#define PE_DIR_IMPORT  1
#define PE_DIR_IAT     12
#define PE_NUM_DIRS    16

typedef PACKED struct {
    u16 e_magic; u16 e_cblp,e_cp,e_crlc,e_cparhdr;
    u16 e_minalloc,e_maxalloc,e_ss,e_sp,e_csum,e_ip,e_cs,e_lfarlc,e_ovno;
    u16 e_res[4]; u16 e_oemid,e_oeminfo; u16 e_res2[10];
    u32 e_lfanew;
} PE_DosHeader;

typedef PACKED struct {
    u16 Machine,NumberOfSections; u32 TimeDateStamp,PointerToSymbolTable,NumberOfSymbols;
    u16 SizeOfOptionalHeader,Characteristics;
} PE_FileHeader;

typedef PACKED struct { u32 VirtualAddress,Size; } PE_DataDirectory;

typedef PACKED struct {
    u16 Magic; u8 MajorLinkerVersion,MinorLinkerVersion;
    u32 SizeOfCode,SizeOfInitializedData,SizeOfUninitializedData;
    u32 AddressOfEntryPoint,BaseOfCode;
    u64 ImageBase; u32 SectionAlignment,FileAlignment;
    u16 MajorOSVersion,MinorOSVersion,MajorImageVersion,MinorImageVersion;
    u16 MajorSubsystemVersion,MinorSubsystemVersion;
    u32 Win32VersionValue,SizeOfImage,SizeOfHeaders,CheckSum;
    u16 Subsystem,DllCharacteristics;
    u64 SizeOfStackReserve,SizeOfStackCommit,SizeOfHeapReserve,SizeOfHeapCommit;
    u32 LoaderFlags,NumberOfRvaAndSizes;
    PE_DataDirectory DataDirectory[PE_NUM_DIRS];
} PE_OptionalHeader64;

typedef PACKED struct {
    char Name[8]; u32 VirtualSize,VirtualAddress,SizeOfRawData,PointerToRawData;
    u32 PointerToRelocations,PointerToLinenumbers;
    u16 NumberOfRelocations,NumberOfLinenumbers; u32 Characteristics;
} PE_SectionHeader;

typedef struct { StrView dll_name, func_name; u32 iat_offset; } PE_ImportEntry;

#define PE_MAX_IMPORTS  256
#define PE_MAX_EXPORTS  256

typedef struct {
    Arena  *arena;
    Target  target;
    u8     *text_buf;
    usize   text_size, text_cap;
    u8     *data_buf;
    usize   data_size;
    u8     *rdata_buf;
    usize   rdata_size, rdata_cap;
    PE_ImportEntry imports[PE_MAX_IMPORTS];
    u32            import_count;
    StrView  exports[PE_MAX_EXPORTS];
    u32      export_offsets[PE_MAX_EXPORTS];
    u32      export_count;
    u64      iat_slots[PE_MAX_IMPORTS];
    u32  section_alignment, file_alignment;
    u64  image_base;
} PE_Context;

PE_Context *pe_context_new(Arena *arena, Target target);
u32  pe_declare_import(PE_Context *ctx, StrView dll_name, StrView func_name);
u32  pe_emit_code(PE_Context *ctx, const u8 *bytes, usize len);
u32  pe_emit_rdata(PE_Context *ctx, const void *data, usize len, usize align);
void pe_add_export(PE_Context *ctx, StrView name, u32 code_rva);
u32  pe_iat_rva(PE_Context *ctx, u32 import_idx);

typedef struct { bool had_error; const char *msg; } PEResult;
PEResult pe_compile_module(PE_Context *ctx, IRModule *module);
PEResult pe_write(PE_Context *ctx, const char *path);
