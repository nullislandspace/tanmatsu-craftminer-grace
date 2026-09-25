// =====================================================================
//  SynthMiner  --  the UI in the player's language (see i18n.h)
// =====================================================================

#include "i18n/i18n.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/psram.h"

static sm_lang_t           s_lang  = SM_LANG_EN;
static char const* const*  s_baked = SM_STRINGS[SM_LANG_EN];

// An override file read off the SD card: the file itself, chopped into
// NUL-terminated lines, and a pointer per string into it (NULL where
// the file said nothing). Freed and rebuilt when the language changes.
static char*       s_over_file;
static char const* s_over[SM_STR_COUNT];

char const* i18n_text(sm_str_t s) {
    if ((unsigned)s >= SM_STR_COUNT) return "";
    if (s_over[s] != NULL) return s_over[s];
    return s_baked[s];
}

sm_lang_t i18n_language(void) {
    return s_lang;
}

char const* i18n_language_name(sm_lang_t lang) {
    return (unsigned)lang < SM_LANG_COUNT ? SM_LANG_NAMES[lang] : "";
}

char const* i18n_language_code(sm_lang_t lang) {
    return (unsigned)lang < SM_LANG_COUNT ? SM_LANG_CODES[lang] : "";
}

bool i18n_language_from_code(char const* code, sm_lang_t* out) {
    if (code == NULL) return false;
    for (int i = 0; i < SM_LANG_COUNT; i++) {
        if (strcmp(code, SM_LANG_CODES[i]) == 0) {
            *out = (sm_lang_t)i;
            return true;
        }
    }
    return false;
}

void i18n_set_language(sm_lang_t lang) {
    if ((unsigned)lang >= SM_LANG_COUNT) lang = SM_LANG_EN;
    s_lang  = lang;
    s_baked = SM_STRINGS[lang];
    // The overrides belonged to the language we have just left.
    sm_free(s_over_file);
    s_over_file = NULL;
    memset(s_over, 0, sizeof s_over);
}

// =====================================================================
//  Formatting
// ---------------------------------------------------------------------
//  The C library is only ever handed one value and one specifier, so
//  nothing here needs a libc that understands `%2$s`. What each value
//  IS comes from the English string; the translation may move a value
//  or pad it differently, and can do nothing else. See i18n.h.
// =====================================================================

typedef enum {
    K_NONE = 0,
    K_INT,
    K_UINT,
    K_LONG,
    K_ULONG,
    K_LLONG,
    K_ULLONG,
    K_DOUBLE,
    K_STR,
    K_CHAR,
} argkind_t;

typedef struct {
    argkind_t kind;
    char      len[3];  // the length modifier as written: "", "l", "ll", "z"
    char      conv;    // d i u x X o f F e E g G s c
} spec_t;

typedef union {
    int                i;
    unsigned int       u;
    long               l;
    unsigned long      ul;
    long long          ll;
    unsigned long long ull;
    double             d;
    char const*        s;
} argval_t;

// Walk a specifier that starts at `p` (which points just past the '%'),
// filling in `sp`. Returns where it ended, or NULL if it is not one.
static char const* parse_spec(char const* p, spec_t* sp) {
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    int n = 0;
    while ((*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't') && n < 2) {
        sp->len[n++] = *p++;
    }
    sp->len[n] = '\0';
    sp->conv   = *p;
    bool const is_ll = strcmp(sp->len, "ll") == 0 || strcmp(sp->len, "j") == 0;
    bool const is_l  = strcmp(sp->len, "l") == 0 || strcmp(sp->len, "z") == 0 ||
                      strcmp(sp->len, "t") == 0;
    switch (sp->conv) {
        case 'd':
        case 'i': sp->kind = is_ll ? K_LLONG : is_l ? K_LONG : K_INT; break;
        case 'u':
        case 'x':
        case 'X':
        case 'o': sp->kind = is_ll ? K_ULLONG : is_l ? K_ULONG : K_UINT; break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': sp->kind = K_DOUBLE; break;
        case 's': sp->kind = K_STR; break;
        case 'c': sp->kind = K_CHAR; break;
        default: return NULL;  // '%' with nothing we format after it
    }
    return p + 1;
}

// The English string says what the values are and in what order they
// arrive. Returns how many there are.
static int reference_specs(sm_str_t s, spec_t out[I18N_FMT_MAX_ARGS]) {
    char const* p = SM_STRINGS[SM_LANG_EN][s];
    int         n = 0;
    while (*p != '\0') {
        if (*p != '%') {
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            p++;
            continue;
        }
        spec_t      sp   = {0};
        char const* next = parse_spec(p, &sp);
        if (next == NULL) break;
        if (n < I18N_FMT_MAX_ARGS) out[n++] = sp;
        p = next;
    }
    return n;
}

static void append(char* buf, size_t cap, size_t* pos, char const* text, size_t n) {
    for (size_t i = 0; i < n; i++, (*pos)++) {
        if (*pos + 1 < cap) buf[*pos] = text[i];
    }
}

// One value, with the flags and width the translation asked for and the
// length and conversion the English string dictates.
static void append_value(char* buf, size_t cap, size_t* pos, char const* flags, size_t flags_n,
                         spec_t const* sp, argval_t const* v) {
    char spec[32];
    size_t k = 0;
    spec[k++] = '%';
    for (size_t i = 0; i < flags_n && k < sizeof spec - 6; i++) spec[k++] = flags[i];
    for (size_t i = 0; sp->len[i] != '\0' && k < sizeof spec - 2; i++) spec[k++] = sp->len[i];
    spec[k++] = sp->conv;
    spec[k]   = '\0';

    char*  at   = (*pos < cap) ? buf + *pos : NULL;
    size_t room = (*pos < cap) ? cap - *pos : 0;
    int    n    = 0;
    switch (sp->kind) {
        case K_INT: n = snprintf(at, room, spec, v->i); break;
        case K_UINT: n = snprintf(at, room, spec, v->u); break;
        case K_LONG: n = snprintf(at, room, spec, v->l); break;
        case K_ULONG: n = snprintf(at, room, spec, v->ul); break;
        case K_LLONG: n = snprintf(at, room, spec, v->ll); break;
        case K_ULLONG: n = snprintf(at, room, spec, v->ull); break;
        case K_DOUBLE: n = snprintf(at, room, spec, v->d); break;
        case K_STR: n = snprintf(at, room, spec, v->s != NULL ? v->s : ""); break;
        case K_CHAR: n = snprintf(at, room, spec, v->i); break;
        default: break;
    }
    if (n > 0) *pos += (size_t)n;
}

int i18n_vfmt(char* buf, size_t cap, sm_str_t s, va_list ap) {
    if (cap > 0) buf[0] = '\0';
    if ((unsigned)s >= SM_STR_COUNT) return 0;

    spec_t    specs[I18N_FMT_MAX_ARGS];
    int const n_args = reference_specs(s, specs);
    argval_t  vals[I18N_FMT_MAX_ARGS];
    memset(vals, 0, sizeof vals);
    for (int i = 0; i < n_args; i++) {
        switch (specs[i].kind) {
            case K_INT:
            case K_CHAR: vals[i].i = va_arg(ap, int); break;
            case K_UINT: vals[i].u = va_arg(ap, unsigned int); break;
            case K_LONG: vals[i].l = va_arg(ap, long); break;
            case K_ULONG: vals[i].ul = va_arg(ap, unsigned long); break;
            case K_LLONG: vals[i].ll = va_arg(ap, long long); break;
            case K_ULLONG: vals[i].ull = va_arg(ap, unsigned long long); break;
            case K_DOUBLE: vals[i].d = va_arg(ap, double); break;
            case K_STR: vals[i].s = va_arg(ap, char const*); break;
            default: break;
        }
    }

    char const* p    = i18n_text(s);
    size_t      pos  = 0;
    int         next = 0;  // the value a specifier gets when it does not say
    while (*p != '\0') {
        if (*p != '%') {
            char const* run = p;
            while (*p != '\0' && *p != '%') p++;
            append(buf, cap, &pos, run, (size_t)(p - run));
            continue;
        }
        p++;
        if (*p == '%') {
            append(buf, cap, &pos, "%", 1);
            p++;
            continue;
        }
        // `%N$...`: which value this is. Digits NOT followed by '$' are a
        // width, and belong to the specifier instead.
        int         which = -1;
        char const* digits = p;
        while (*digits >= '0' && *digits <= '9') digits++;
        if (digits != p && *digits == '$') {
            which = 0;
            for (char const* d = p; d < digits; d++) which = which * 10 + (*d - '0');
            which--;
            p = digits + 1;
        }
        char const* flags = p;
        spec_t      sp    = {0};
        char const* end   = parse_spec(p, &sp);
        if (end == NULL) {
            // Not a specifier at all: show it as typed rather than eat it.
            append(buf, cap, &pos, "%", 1);
            continue;
        }
        if (which < 0) which = next++;
        // Everything up to the length modifier is the translation's to
        // choose; the rest is English's. `end - 1` is the conversion.
        size_t flags_n = (size_t)(end - 1 - flags);
        while (flags_n > 0 && (flags[flags_n - 1] == 'l' || flags[flags_n - 1] == 'h' ||
                               flags[flags_n - 1] == 'z' || flags[flags_n - 1] == 'j' ||
                               flags[flags_n - 1] == 't')) {
            flags_n--;
        }
        if (which >= 0 && which < n_args) {
            append_value(buf, cap, &pos, flags, flags_n, &specs[which], &vals[which]);
        }
        // A value English does not have is dropped: a lang file on the
        // card is not allowed to decide what is on the stack.
        p = end;
    }
    if (cap > 0) buf[pos < cap ? pos : cap - 1] = '\0';
    return (int)pos;
}

int i18n_fmt(char* buf, size_t cap, sm_str_t s, ...) {
    va_list ap;
    va_start(ap, s);
    int const n = i18n_vfmt(buf, cap, s, ap);
    va_end(ap);
    return n;
}

// =====================================================================
//  Overrides from the SD card
// =====================================================================

// The key half of a `key = value` line, matched against the generated
// key table. -1 for a key this build does not have.
static int key_index(char const* key, size_t n) {
    for (int i = 0; i < SM_STR_COUNT; i++) {
        if (strncmp(SM_STR_KEYS[i], key, n) == 0 && SM_STR_KEYS[i][n] == '\0') return i;
    }
    return -1;
}

// `\n`, `\\` and `\s` in place, the same three make_lang.py understands.
static void unescape(char* s) {
    char* w = s;
    for (char* r = s; *r != '\0'; r++) {
        if (*r != '\\' || r[1] == '\0') {
            *w++ = *r;
            continue;
        }
        r++;
        switch (*r) {
            case 'n': *w++ = '\n'; break;
            case 's': *w++ = ' '; break;
            case '\\': *w++ = '\\'; break;
            default:
                *w++ = '\\';
                *w++ = *r;
                break;
        }
    }
    *w = '\0';
}

void i18n_load_overrides(char const* dir) {
    sm_free(s_over_file);
    s_over_file = NULL;
    memset(s_over, 0, sizeof s_over);
    if (dir == NULL) return;

    char path[192];
    snprintf(path, sizeof path, "%s/lang/%s.txt", dir, i18n_language_code(s_lang));
    FILE* f = fopen(path, "rb");
    if (f == NULL) return;  // the ordinary case: no override file at all

    fseek(f, 0, SEEK_END);
    long const size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fclose(f);
        return;
    }
    char* buf = (char*)sm_alloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    size_t const got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    s_over_file = buf;

    char* line = buf;
    while (*line != '\0') {
        char* end = line + strcspn(line, "\r\n");
        char  was = *end;
        *end      = '\0';

        char* hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        char* eq = strchr(line, '=');
        if (eq != NULL) {
            char* key_end = eq;
            while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
            while (*line == ' ' || *line == '\t') line++;
            char* val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            char* val_end = val + strlen(val);
            while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
            *val_end = '\0';
            int const idx = key_index(line, (size_t)(key_end - line));
            if (idx >= 0) {
                unescape(val);
                s_over[idx] = val;
            }
        }
        if (was == '\0') break;
        line = end + 1;
    }
}

#ifdef SM_HOST
void i18n_test_override(sm_str_t s, char const* text) {
    if ((unsigned)s < SM_STR_COUNT) s_over[s] = text;
}
#endif
