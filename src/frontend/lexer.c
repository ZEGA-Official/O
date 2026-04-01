// ============================================================
//  O Language Compiler — o_lexer.c
//  Lexer implementation
//  Z-TEAM | C23
// ============================================================
#include "frontend/lexer.h"
#include <ctype.h>

static const struct { const char *kw; TokKind kind; } KEYWORDS[] = {
    {"fn",TOK_KW_FN},{"let",TOK_KW_LET},{"const",TOK_KW_CONST},
    {"return",TOK_KW_RETURN},{"if",TOK_KW_IF},{"else",TOK_KW_ELSE},
    {"while",TOK_KW_WHILE},{"for",TOK_KW_FOR},{"break",TOK_KW_BREAK},
    {"continue",TOK_KW_CONTINUE},{"struct",TOK_KW_STRUCT},{"union",TOK_KW_UNION},
    {"enum",TOK_KW_ENUM},{"import",TOK_KW_IMPORT},{"extern",TOK_KW_EXTERN},
    {"defer",TOK_KW_DEFER},{"as",TOK_KW_AS},{"sizeof",TOK_KW_SIZEOF},
    {"alignof",TOK_KW_ALIGNOF},{"null",TOK_KW_NULL},{"true",TOK_KW_TRUE},
    {"false",TOK_KW_FALSE},{"inline",TOK_KW_INLINE},{"volatile",TOK_KW_VOLATILE},
    {"mut",TOK_KW_MUT},{"void",TOK_KW_VOID},{"bool",TOK_KW_BOOL},
    {"i8",TOK_KW_I8},{"i16",TOK_KW_I16},{"i32",TOK_KW_I32},{"i64",TOK_KW_I64},
    {"u8",TOK_KW_U8},{"u16",TOK_KW_U16},{"u32",TOK_KW_U32},{"u64",TOK_KW_U64},
    {"f32",TOK_KW_F32},{"f64",TOK_KW_F64},{"isize",TOK_KW_ISIZE},{"usize",TOK_KW_USIZE},
};

static inline char cur(const Lexer *l) { return l->pos < l->src_len ? l->src[l->pos] : '\0'; }
static inline char peek1(const Lexer *l) { return (l->pos+1)<l->src_len ? l->src[l->pos+1] : '\0'; }
static inline void advance(Lexer *l) {
    if (l->pos < l->src_len) {
        if (l->src[l->pos]=='\n'){l->line++;l->col=1;} else l->col++;
        l->pos++;
    }
}
static inline SrcLoc here(const Lexer *l) {
    return (SrcLoc){.file=l->filename,.line=l->line,.col=l->col};
}

static void skip_whitespace(Lexer *l) {
    for (;;) {
        while (l->pos<l->src_len && isspace((u8)cur(l))) advance(l);
        if (cur(l)=='/'&&peek1(l)=='/') { while(l->pos<l->src_len&&cur(l)!='\n') advance(l); continue; }
        if (cur(l)=='/'&&peek1(l)=='*') {
            advance(l); advance(l);
            while (l->pos<l->src_len) {
                if (cur(l)=='*'&&peek1(l)=='/') { advance(l); advance(l); break; }
                advance(l);
            }
            continue;
        }
        break;
    }
}

static TokKind classify_ident(const char *ptr, usize len) {
    for (usize i=0; i<ARRAY_LEN(KEYWORDS); i++) {
        const char *kw=KEYWORDS[i].kw;
        if (strlen(kw)==len && memcmp(kw,ptr,len)==0) return KEYWORDS[i].kind;
    }
    return TOK_IDENT;
}

static Token lex_ident(Lexer *l) {
    SrcLoc loc=here(l); const char *start=l->src+l->pos;
    while (l->pos<l->src_len && (isalnum((u8)cur(l))||cur(l)=='_')) advance(l);
    usize len=(usize)(l->src+l->pos-start);
    return (Token){.kind=classify_ident(start,len),.text={.ptr=start,.len=len},.loc=loc};
}

static u64 parse_escape(Lexer *l) {
    advance(l); char c=cur(l); advance(l);
    switch(c) {
        case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
        case '0': return '\0'; case '\\': return '\\'; case '\'': return '\'';
        case '"': return '"';
        case 'x': { u64 v=0; for(int i=0;i<2;i++){char h=cur(l);advance(l);
            if(h>='0'&&h<='9') v=v*16+(h-'0');
            else if(h>='a') v=v*16+(h-'a'+10); else v=v*16+(h-'A'+10);} return v; }
        default: o_diag_error(here(l),"unknown escape '\\%c'",c); return c;
    }
}

static Token lex_string(Lexer *l) {
    SrcLoc loc=here(l); advance(l);
    usize raw_max=l->src_len-l->pos+1;
    char *buf=(char *)arena_alloc_aligned(l->arena,raw_max,1);
    if (!buf) return (Token){.kind=TOK_ERROR,.loc=loc};
    usize wi=0;
    while (l->pos<l->src_len && cur(l)!='"') {
        if (cur(l)=='\\') { buf[wi++]=(char)parse_escape(l); }
        else { buf[wi++]=cur(l); advance(l); }
    }
    buf[wi]='\0';
    if (cur(l)=='"') advance(l);
    return (Token){.kind=TOK_STRING_LIT,.text={.ptr=buf,.len=wi},.loc=loc};
}

static Token lex_char(Lexer *l) {
    SrcLoc loc=here(l); advance(l);
    u64 val = (cur(l)=='\\') ? parse_escape(l) : (u64)(u8)cur(l);
    if (cur(l)!='\\') advance(l);
    if (cur(l)=='\'') advance(l);
    return (Token){.kind=TOK_CHAR_LIT,.text={.ptr=l->src+l->pos,0},.loc=loc,.int_val=val};
}

static Token lex_number(Lexer *l) {
    SrcLoc loc=here(l); const char *start=l->src+l->pos;
    u64 val=0; bool is_float=false;
    if (cur(l)=='0') {
        advance(l);
        if (cur(l)=='x'||cur(l)=='X') {
            advance(l);
            while (isxdigit((u8)cur(l))||cur(l)=='_') {
                if (cur(l)!='_') { val*=16; char c=cur(l);
                    if(c>='0'&&c<='9') val+=c-'0';
                    else if(c>='a') val+=c-'a'+10; else val+=c-'A'+10; }
                advance(l);
            }
        } else if (cur(l)=='b'||cur(l)=='B') {
            advance(l);
            while(cur(l)=='0'||cur(l)=='1'||cur(l)=='_'){if(cur(l)!='_')val=val*2+(cur(l)-'0');advance(l);}
        } else {
            while(isdigit((u8)cur(l))||cur(l)=='_'){if(cur(l)!='_')val=val*10+(cur(l)-'0');advance(l);}
        }
    } else {
        while(isdigit((u8)cur(l))||cur(l)=='_'){if(cur(l)!='_')val=val*10+(cur(l)-'0');advance(l);}
    }
    if (cur(l)=='.'&&peek1(l)!='.') { is_float=true; advance(l); while(isdigit((u8)cur(l)))advance(l); }
    if (cur(l)=='e'||cur(l)=='E') { is_float=true; advance(l);
        if(cur(l)=='+'||cur(l)=='-')advance(l); while(isdigit((u8)cur(l)))advance(l); }
    usize len=(usize)(l->src+l->pos-start);
    Token tok={.text={.ptr=start,.len=len},.loc=loc};
    if (is_float) { tok.kind=TOK_FLOAT_LIT; char tmp[64]={0}; memcpy(tmp,start,MIN(len,63)); tok.flt_val=strtod(tmp,NULL); }
    else { tok.kind=TOK_INT_LIT; tok.int_val=val; }
    return tok;
}

void lexer_init(Lexer *l, Arena *arena, const char *filename, const char *src, usize src_len) {
    *l=(Lexer){.src=src,.src_len=src_len,.pos=0,.line=1,.col=1,.filename=filename,.arena=arena};
}

#define ONE(kind_)  (Token){.kind=(kind_), .text={.ptr=start,1}, .loc=loc}
#define TWO(kind_)  (advance(l), (Token){.kind=(kind_), .text={.ptr=start,2}, .loc=loc})

Token lexer_next(Lexer *l) {
    skip_whitespace(l);
    if (l->pos>=l->src_len) return (Token){.kind=TOK_EOF,.loc=here(l)};
    SrcLoc loc=here(l); const char *start=l->src+l->pos; char c=cur(l);
    if (isalpha((u8)c)||c=='_') return lex_ident(l);
    if (isdigit((u8)c)) return lex_number(l);
    if (c=='"') return lex_string(l);
    if (c=='\'') return lex_char(l);
    advance(l);
    switch(c) {
        case '(': return ONE(TOK_LPAREN); case ')': return ONE(TOK_RPAREN);
        case '{': return ONE(TOK_LBRACE); case '}': return ONE(TOK_RBRACE);
        case '[': return ONE(TOK_LBRACKET); case ']': return ONE(TOK_RBRACKET);
        case ';': return ONE(TOK_SEMI); case ',': return ONE(TOK_COMMA);
        case '~': return ONE(TOK_TILDE); case '@': return ONE(TOK_AT);
        case '#': return ONE(TOK_HASH); case '?': return ONE(TOK_QUESTION);
        case ':': return ONE(TOK_COLON);
        case '.':
            if (cur(l)=='.') { advance(l);
                if (cur(l)=='.') { advance(l); return (Token){.kind=TOK_DOTDOTDOT,.text={.ptr=start,.len=3},.loc=loc}; }
                return (Token){.kind=TOK_DOTDOT,.text={.ptr=start,.len=2},.loc=loc}; }
            return ONE(TOK_DOT);
        case '+': if(cur(l)=='+')return TWO(TOK_PLUS_PLUS); if(cur(l)=='=')return TWO(TOK_PLUS_ASSIGN); return ONE(TOK_PLUS);
        case '-': if(cur(l)=='-')return TWO(TOK_MINUS_MINUS); if(cur(l)=='>')return TWO(TOK_ARROW); if(cur(l)=='=')return TWO(TOK_MINUS_ASSIGN); return ONE(TOK_MINUS);
        case '*': if(cur(l)=='=')return TWO(TOK_STAR_ASSIGN); return ONE(TOK_STAR);
        case '/': if(cur(l)=='=')return TWO(TOK_SLASH_ASSIGN); return ONE(TOK_SLASH);
        case '%': if(cur(l)=='=')return TWO(TOK_PERCENT_ASSIGN); return ONE(TOK_PERCENT);
        case '&': if(cur(l)=='&')return TWO(TOK_AMP_AMP); if(cur(l)=='=')return TWO(TOK_AMP_ASSIGN); return ONE(TOK_AMP);
        case '|': if(cur(l)=='|')return TWO(TOK_PIPE_PIPE); if(cur(l)=='=')return TWO(TOK_PIPE_ASSIGN); return ONE(TOK_PIPE);
        case '^': if(cur(l)=='=')return TWO(TOK_CARET_ASSIGN); return ONE(TOK_CARET);
        case '!': if(cur(l)=='=')return TWO(TOK_NEQ); return ONE(TOK_BANG);
        case '=': if(cur(l)=='=')return TWO(TOK_EQ); if(cur(l)=='>')return TWO(TOK_FAT_ARROW); return ONE(TOK_ASSIGN);
        case '<':
            if(cur(l)=='<'){advance(l);if(cur(l)=='=')return(advance(l),(Token){.kind=TOK_LSHIFT_ASSIGN,.text={.ptr=start,.len=3},.loc=loc});return(Token){.kind=TOK_LSHIFT,.text={.ptr=start,.len=2},.loc=loc};}
            if(cur(l)=='=')return TWO(TOK_LE); return ONE(TOK_LT);
        case '>':
            if(cur(l)=='>'){advance(l);if(cur(l)=='=')return(advance(l),(Token){.kind=TOK_RSHIFT_ASSIGN,.text={.ptr=start,.len=3},.loc=loc});return(Token){.kind=TOK_RSHIFT,.text={.ptr=start,.len=2},.loc=loc};}
            if(cur(l)=='=')return TWO(TOK_GE); return ONE(TOK_GT);
        default:
            o_diag_error(loc,"unexpected character '%c' (0x%02x)",c,(u8)c);
            l->error_count++;
            return (Token){.kind=TOK_ERROR,.text={.ptr=start,1},.loc=loc};
    }
}

typedef struct { usize pos; u32 line,col; } LexerState;

Token lexer_peek(Lexer *l) {
    LexerState s={l->pos,l->line,l->col};
    Token t=lexer_next(l);
    l->pos=s.pos; l->line=s.line; l->col=s.col;
    return t;
}

const char *tok_kind_str(TokKind k) {
    static const char *n[TOK_COUNT]={
        [TOK_INT_LIT]="<int>",[TOK_FLOAT_LIT]="<float>",
        [TOK_STRING_LIT]="<str>",[TOK_CHAR_LIT]="<chr>",[TOK_IDENT]="<id>",
        [TOK_KW_FN]="fn",[TOK_KW_LET]="let",[TOK_KW_CONST]="const",
        [TOK_KW_RETURN]="return",[TOK_KW_IF]="if",[TOK_KW_ELSE]="else",
        [TOK_KW_WHILE]="while",[TOK_KW_FOR]="for",[TOK_KW_BREAK]="break",
        [TOK_KW_CONTINUE]="continue",[TOK_KW_EXTERN]="extern",
        [TOK_KW_STRUCT]="struct",[TOK_KW_ENUM]="enum",
        [TOK_KW_AS]="as",[TOK_KW_SIZEOF]="sizeof",[TOK_KW_MUT]="mut",
        [TOK_KW_NULL]="null",[TOK_KW_TRUE]="true",[TOK_KW_FALSE]="false",
        [TOK_KW_VOID]="void",[TOK_KW_BOOL]="bool",
        [TOK_KW_I8]="i8",[TOK_KW_I16]="i16",[TOK_KW_I32]="i32",[TOK_KW_I64]="i64",
        [TOK_KW_U8]="u8",[TOK_KW_U16]="u16",[TOK_KW_U32]="u32",[TOK_KW_U64]="u64",
        [TOK_KW_F32]="f32",[TOK_KW_F64]="f64",[TOK_KW_ISIZE]="isize",[TOK_KW_USIZE]="usize",
        [TOK_LPAREN]="(",[TOK_RPAREN]=")",[TOK_LBRACE]="{",[TOK_RBRACE]="}",
        [TOK_LBRACKET]="[",[TOK_RBRACKET]="]",[TOK_SEMI]=";",[TOK_COLON]=":",
        [TOK_COMMA]=",",[TOK_DOT]=".",[TOK_DOTDOT]="..",[TOK_DOTDOTDOT]="...",[TOK_ARROW]="->",
        [TOK_PLUS_PLUS]="++",[TOK_MINUS_MINUS]="--",
        [TOK_PLUS]="+",[TOK_MINUS]="-",[TOK_STAR]="*",[TOK_SLASH]="/",[TOK_PERCENT]="%",
        [TOK_AMP]="&",[TOK_PIPE]="|",[TOK_CARET]="^",[TOK_TILDE]="~",[TOK_BANG]="!",
        [TOK_LSHIFT]="<<",[TOK_RSHIFT]=">>",[TOK_AMP_AMP]="&&",[TOK_PIPE_PIPE]="||",
        [TOK_EQ]="==",[TOK_NEQ]="!=",[TOK_LT]="<",[TOK_GT]=">",[TOK_LE]="<=",[TOK_GE]=">=",
        [TOK_ASSIGN]="=",[TOK_PLUS_ASSIGN]="+=",[TOK_MINUS_ASSIGN]="-=",
        [TOK_EOF]="<eof>",[TOK_ERROR]="<err>",
    };
    return (k<TOK_COUNT&&n[k]) ? n[k] : "<?>";
}
bool tok_is_assign_op(TokKind k){return k==TOK_ASSIGN||k==TOK_PLUS_ASSIGN||k==TOK_MINUS_ASSIGN||k==TOK_STAR_ASSIGN||k==TOK_SLASH_ASSIGN||k==TOK_PERCENT_ASSIGN||k==TOK_AMP_ASSIGN||k==TOK_PIPE_ASSIGN||k==TOK_CARET_ASSIGN||k==TOK_LSHIFT_ASSIGN||k==TOK_RSHIFT_ASSIGN;}
bool tok_is_cmp_op(TokKind k){return k==TOK_EQ||k==TOK_NEQ||k==TOK_LT||k==TOK_GT||k==TOK_LE||k==TOK_GE;}
bool tok_is_type_keyword(TokKind k){return k==TOK_KW_VOID||k==TOK_KW_BOOL||(k>=TOK_KW_I8&&k<=TOK_KW_USIZE);}
