// ============================================================
//  O Language Compiler — include/o_lang.h
//  Umbrella header: #include "o_lang.h" gets everything
//  Z-TEAM | C23
// ============================================================
#pragma once

// ── Core foundation ──────────────────────────────────────────
#include "core/common.h"
#include "core/arena.h"

// ── Frontend ─────────────────────────────────────────────────
#include "frontend/lexer.h"
#include "frontend/ast.h"

// ── Intermediate Representation ───────────────────────────────
#include "ir/ir.h"

// ── Backend ──────────────────────────────────────────────────
#include "backend/target.h"
#include "backend/x64.h"

// ── JIT Engine ────────────────────────────────────────────────
#include "jit/jit.h"

// ── Output Formats ────────────────────────────────────────────
#include "lib/elf/aot.h"
#include "lib/elf/exec.h"
#include "lib/windows/pe.h"
#include "lib/iso/iso.h"
