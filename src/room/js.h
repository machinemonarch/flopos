/* ===========================================================================
 * js.h -- the script room's engine: what its parts share.
 *
 * The engine runs JavaScript for browzer, in a room of its own (a walled
 * 4 MB of memory) where it can reach nothing but its own memory and one
 * door to browzer.  It is built twice: for the room, and as a program on
 * the PC that builds FLOPOS, so test/jstest can try it in seconds.
 *
 * How a script runs: the lexer (lex.c) cuts the text into words, the
 * compiler (compile.c) follows the grammar and writes instructions for a
 * simple stack machine (bytecode), and the interpreter (vm.c) carries them
 * out one at a time, counting as it goes.  Nothing is ever turned into the
 * processor's own instructions, so no memory is ever both written and run.
 *
 * Values live in one heap (gc.c), swept when it fills: anything the
 * script can no longer reach is freed.
 * ===========================================================================
 */
#ifndef JS_H
#define JS_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef long long           s64;
typedef double              f64;

/* ---- what the engine asks of the place it runs in (room.c, or the test
 * program) ------------------------------------------------------------------ */
u32  host_ms(void);                      /* a clock in milliseconds         */
/* A request through the door: in is len bytes, the answer is written to
 * out (cap bytes); returns its length, or -1 if refused. */
int  host_door(const u8 *in, u32 len, u8 *out, u32 cap);

/* ---- memory helpers (no C library here) -------------------------------- */
void *memset(void *d, int c, __SIZE_TYPE__ n);
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memmove(void *d, const void *s, __SIZE_TYPE__ n);
int   js_strlen(const char *s);

/* ---- values -------------------------------------------------------------- */
typedef struct Str Str;
typedef struct Obj Obj;
typedef struct Env Env;
typedef struct Proto Proto;
typedef struct Props Props;
typedef struct Vm Vm;

enum { T_UNDEF, T_NULL, T_BOOL, T_NUM, T_STR, T_OBJ };

typedef struct {
    u32 t;
    union { f64 n; u32 b; Str *s; Obj *o; } u;
} Val;                                   /* 12 bytes on this machine         */

/* Every block in the heap starts with this. */
typedef struct {
    u32 size;                            /* the whole block, bytes          */
    u8  kind, mark;
    u16 extra;
} GcHdr;

enum { K_FREE, K_STR, K_OBJ, K_PROPS, K_VALS, K_ENV, K_PROTO, K_BYTES };

struct Str {
    GcHdr h;
    u32 len, hash;                       /* hash 0: not worked out yet      */
    u8  interned, pad[3];
    u16 c[];                             /* UTF-16                          */
};

/* A property: a name, a value, and whether it is a getter/setter pair. */
#define P_ACCESSOR 1
#define P_HIDDEN   2                     /* not listed by for-in or keys    */
typedef struct { Str *k; Val v; Val set; u32 flags; } Prop;

struct Props {
    GcHdr h;
    u32 n, cap, live;                    /* used slots, room, not deleted   */
    Prop p[];                            /* in the order they were made; a
                                          * u16 index of 2*cap follows      */
};

typedef struct {
    GcHdr h;
    u32 n;
    Val v[];
} Vals;

enum {
    C_OBJECT, C_ARRAY, C_FUNC, C_NATIVE, C_BOUND, C_ERROR, C_REGEXP, C_DATE,
    C_ARGS, C_PROMISE, C_HOST, C_STYLE, C_CLASSLIST, C_BOXED, C_ITER,
    C_ASYNC                              /* an async function, paused at an await */
};

typedef Val (*NativeFn)(Vm *vm, Val self, Val *args, int argc, u32 magic);

struct Obj {
    GcHdr h;
    u8  cls, sealed, flags, pad;
    Obj   *proto;
    Props *props;
    union {
        struct { Vals *items; u32 len; } arr;
        struct { Proto *fn; Env *env; Val self; } fn;      /* self: an arrow's this */
        struct { NativeFn f; u32 magic; Str *name; u32 nargs; } nat;
        struct { Obj *target; Val self; Vals *args; } bound;
        struct { Str *src; u32 flags; void *prog; u32 last; } re;
        struct { f64 t; } date;
        struct { u32 node; } host;
        struct { u32 state; Val value; Vals *reactions; } prom;
        struct { Val v; } boxed;
        struct { Val of; u32 i; Vals *keys; } iter;
        struct { Val prom; Vals *stack; void *fr; } co;    /* fr: its Frame, kept in a K_BYTES block */
    } u;
};

#define OF_CALLABLE 1                    /* flags                           */
#define OF_ARROW    2
#define OF_CTOR     4

struct Env {
    GcHdr h;
    Env *parent;
    u32 n;
    Val v[];
};

/* A compiled function: its instructions, its constants, and the functions
 * written inside it. */
struct Proto {
    GcHdr h;
    u8  *code;
    u32  ncode;
    Vals *consts;                        /* numbers and strings              */
    Proto **kids;
    u32  nkids;
    u16  nparams, nslots;
    u16  args_slot;                      /* 0xFFFF: no `arguments`          */
    u16  self_slot;                      /* a named function expression's own name */
    u8   arrow, uses_this, pad[2];
    Str *name;
    u8  *lines;                          /* not kept: kept small            */
};

/* ---- making values -------------------------------------------------------- */
static inline Val v_undef(void) { Val v; v.t = T_UNDEF; v.u.n = 0; return v; }
static inline Val v_null(void)  { Val v; v.t = T_NULL;  v.u.n = 0; return v; }
static inline Val v_bool(int b) { Val v; v.t = T_BOOL;  v.u.n = 0; v.u.b = !!b; return v; }
static inline Val v_num(f64 n)  { Val v; v.t = T_NUM;   v.u.n = n; return v; }
static inline Val v_str(Str *s) { Val v; v.t = T_STR;   v.u.n = 0; v.u.s = s; return v; }
static inline Val v_obj(Obj *o) { Val v; v.t = T_OBJ;   v.u.n = 0; v.u.o = o; return v; }
#define IS_OBJ(v)  ((v).t == T_OBJ)
#define IS_STR(v)  ((v).t == T_STR)
#define IS_NUM(v)  ((v).t == T_NUM)
#define IS_NULLISH(v) ((v).t <= T_NULL)

/* ---- the machine ------------------------------------------------------------ */
#define VM_STACK   16384                 /* values                           */
#define VM_FRAMES  256                   /* calls deep                       */
#define VM_TRIES   8                     /* try blocks open in one call      */
#define VM_TEMPS   256

typedef struct { u32 pc, sp; } Try;

typedef struct {
    Proto *p;
    Obj   *fn;
    Env   *env;
    Val    self;
    u32    pc, base;                     /* base: where its stack starts     */
    u32    ntry;
    Try    tries[VM_TRIES];
    u8     is_new, stop;                 /* stop: return to the C caller     */
    Obj   *co;                           /* an async function's own state, or 0 */
} Frame;

typedef struct { Obj *fn; Val *args; u32 nargs; f64 due, every; u32 id; } Timer;

struct Vm {
    Val    stack[VM_STACK];
    u32    sp;
    Frame  frames[VM_FRAMES];
    u32    nframes;
    Val    temps[VM_TEMPS];              /* values C code is holding on to   */
    u32    ntemps;

    Obj   *global;
    Obj   *object_proto, *function_proto, *array_proto, *string_proto,
          *number_proto, *boolean_proto, *error_proto, *regexp_proto,
          *date_proto, *promise_proto, *iter_proto;
    Obj   *err_ctor[6];                  /* Error, TypeError, RangeError,
                                          * SyntaxError, ReferenceError, EvalError */
    Obj   *element_proto, *document_obj, *style_proto, *classlist_proto, *event_proto;

    int    throwing;                     /* an exception is on its way out   */
    Val    exception;

    u32    steps;                        /* instructions run, this task      */
    u32    deadline;                     /* host_ms() at which to stop       */
    u32    door_ms;                      /* time spent at the door, not counted */
    int    out_of_time, out_of_room, fatal, in_new;
    Obj   *callee;                       /* the native being called         */
    const char *stop_why;

    Obj   *hosts;                        /* node number -> its object        */
    Obj   *microq;                       /* an array: [fn, arg, fn, arg...]  */
    Obj   *timers;                       /* an array of [id, due, every, fn, args] */
    u32    next_timer;
    f64    now_base;                     /* the date, in ms since 1970, at host_ms() == 0 */
    u32    door_count, door_window;      /* door requests this second, and when it began */
};

extern Vm *g_vm;

/* ---- the heap (gc.c) ------------------------------------------------------- */
void  gc_init(u8 *base, u32 size);
void *gc_alloc(u32 size, int kind);      /* 0 if there is no room            */
void  gc_maybe(Vm *vm);                  /* sweep, if the heap is filling    */
void  gc_collect(Vm *vm);
u32   gc_used(void);
void  gc_free(void *block);            /* a block nothing points at, back now */
u32   gc_total(void);
#define TEMP(vm, v) ((vm)->temps[(vm)->ntemps++] = (v))
#define UNTEMP(vm, n) ((vm)->ntemps -= (n))

/* ---- strings (str.c) ------------------------------------------------------- */
Str  *str_new(u32 len);
Str  *str_ascii(const char *s);
Str  *str_utf8(const char *s, u32 n);
Str  *str_from16(const u16 *c, u32 n);
Str  *str_intern(Str *s);
Str  *str_intern16(const u16 *c, u32 n);   /* interned, made only if new */
Str  *atom(const char *s);               /* an interned name                  */
u32   str_hash(Str *s);
int   str_eq(Str *a, Str *b);
int   str_cmp(Str *a, Str *b);
Str  *str_concat(Str *a, Str *b);
Str  *str_slice(Str *s, u32 from, u32 to);
int   str_index(Str *s, Str *what, u32 from);
int   str_is_index(Str *s, u32 *out);    /* "12" -> 12                      */
u32   str_to_utf8(Str *s, char *out, u32 cap);   /* bytes written, no zero  */
int   str_eq_ascii(Str *s, const char *a);
void  intern_sweep(void);
void  intern_mark(void);
extern Str *S_length, *S_prototype, *S_constructor, *S_name, *S_message,
           *S_toString, *S_valueOf, *S_empty, *S_undefined, *S_null,
           *S_true, *S_false, *S_object, *S_function, *S_number, *S_string,
           *S_boolean, *S_callee, *S_lastIndex, *S_index, *S_input,
           *S_get, *S_set, *S_value, *S_writable, *S_enumerable,
           *S_configurable, *S_then, *S_stack, *S_next, *S_done, *S_join,
           *S_NaN, *S_Infinity, *S_symbol, *S_toJSON;
void  str_init_atoms(void);

/* ---- numbers (num.c) --------------------------------------------------------- */
f64   num_parse(const u16 *s, u32 n, int *ok);       /* the whole of it, JS's way */
f64   num_parse_prefix(const u16 *s, u32 n, u32 *used); /* parseFloat's way  */
Str  *num_to_str(f64 v, int radix);
int   num_fmt(f64 v, char *out);         /* shortest form, as JS writes it   */
f64   m_floor(f64 x), m_ceil(f64 x), m_trunc(f64 x), m_round(f64 x);
f64   m_sqrt(f64 x), m_sin(f64 x), m_cos(f64 x), m_tan(f64 x), m_atan2(f64 y, f64 x);
f64   m_exp(f64 x), m_log(f64 x), m_pow(f64 x, f64 y), m_fmod(f64 x, f64 y);
f64   m_nan(void), m_inf(void);
int   m_isnan(f64 x), m_isinf(f64 x);
s32   to_int32(f64 x);
u32   to_uint32(f64 x);

/* ---- objects (obj.c) -------------------------------------------------------- */
Obj  *obj_new(Vm *vm, Obj *proto, int cls);
Obj  *arr_new(Vm *vm, u32 len);
int   arr_push(Vm *vm, Obj *a, Val v);
Val   arr_get(Obj *a, u32 i);
int   arr_set(Vm *vm, Obj *a, u32 i, Val v);
int   arr_set_len(Vm *vm, Obj *a, u32 n);
Prop *prop_find(Obj *o, Str *k);         /* own only                         */
int   prop_put(Vm *vm, Obj *o, Str *k, Val v, u32 flags);
int   prop_del(Obj *o, Str *k);
Obj  *native_new(Vm *vm, const char *name, NativeFn f, int nargs, u32 magic);
void  def_native(Vm *vm, Obj *o, const char *name, NativeFn f, int nargs, u32 magic);
void  def_accessor(Vm *vm, Obj *o, const char *name, NativeFn get, NativeFn set, u32 magic);
void  def_val(Vm *vm, Obj *o, const char *name, Val v);

/* Getting and setting, with prototypes, getters, arrays and strings. */
Val   get_prop(Vm *vm, Val o, Str *k);
Val   get_elem(Vm *vm, Val o, Val k);
int   set_prop(Vm *vm, Val o, Str *k, Val v);
int   set_elem(Vm *vm, Val o, Val k, Val v);
int   has_prop(Vm *vm, Obj *o, Str *k);
int   del_prop(Vm *vm, Val o, Val k);
Obj  *own_keys(Vm *vm, Obj *o, int all);  /* an array of names              */

/* ---- conversions and comparisons (ops.c) ------------------------------------- */
int   truthy(Val v);
f64   to_num(Vm *vm, Val v);
Str  *to_str(Vm *vm, Val v);
Obj  *to_obj(Vm *vm, Val v);
Val   to_prim(Vm *vm, Val v, int hint_str);
Str  *to_key(Vm *vm, Val v);
int   loose_eq(Vm *vm, Val a, Val b);
int   strict_eq(Val a, Val b);
Str  *type_of(Val v);
int   is_callable(Val v);
int   instance_of(Vm *vm, Val v, Val ctor);

/* ---- errors -------------------------------------------------------------------- */
enum { E_ERROR, E_TYPE, E_RANGE, E_SYNTAX, E_REFERENCE, E_EVAL };
Val   throw_err(Vm *vm, int kind, const char *fmt, ...);   /* returns undefined */
Val   throw_val(Vm *vm, Val v);
Obj   *make_err(Vm *vm, int kind, Str *msg);

/* ---- running (vm.c) -------------------------------------------------------------- */
void  vm_init(Vm *vm);
Val   vm_call(Vm *vm, Val fn, Val self, Val *args, int argc);  /* throwing set on error */
Val   vm_construct(Vm *vm, Val fn, Val *args, int argc);
Val   vm_run_script(Vm *vm, Proto *p);
int   vm_tick(Vm *vm);                   /* a step counted; 0 when out of time */
void  vm_run_microtasks(Vm *vm);
int   vm_check_time(Vm *vm);

/* ---- compiling (lex.c, compile.c) ------------------------------------------------- */
/* src is UTF-8.  On a mistake, 0 and *why says what and where. */
Proto *js_compile(Vm *vm, const char *src, u32 len, const char *name, char *why, u32 whycap, int as_function);

/* ---- built-ins (lib.c, lib2.c, json.c, re.c, dom.c) ------------------------- */
void  lib_init(Vm *vm);
void  lib2_init(Vm *vm);
void  json_init(Vm *vm);
void  re_init(Vm *vm);
void  dom_init(Vm *vm);
Val   re_exec(Vm *vm, Obj *re, Str *s, u32 from, int *found, u32 *mstart, u32 *mend);
Obj  *re_new(Vm *vm, Str *src, Str *flags);
Str  *json_stringify(Vm *vm, Val v);
void  promise_resolve_fn(Vm *vm, Obj *p, Val v, int rejected);
Obj  *promise_new(Vm *vm);
Val   promise_then(Vm *vm, Obj *p, Val ok, Val bad);

/* ---- instructions ------------------------------------------------------------- */
enum {
    OP_UNDEF, OP_NULL, OP_TRUE, OP_FALSE, OP_INT8, OP_CONST, OP_POP, OP_DUP,
    OP_DUP2, OP_SWAP, OP_ROT3, OP_ROT4,
    OP_GETVAR, OP_SETVAR, OP_GETGLOBAL, OP_SETGLOBAL, OP_TYPEOF_GLOBAL,
    OP_DECLGLOBAL,
    OP_GETPROP, OP_SETPROP, OP_GETELEM, OP_SETELEM, OP_DELPROP, OP_DELELEM,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_NEG, OP_PLUS, OP_NOT,
    OP_BITNOT, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_INSTANCEOF, OP_IN, OP_TYPEOF, OP_INC, OP_DEC, OP_TOSTR,
    OP_JMP, OP_JF, OP_JT, OP_JFKEEP, OP_JTKEEP, OP_JNNKEEP,
    OP_CLOSURE, OP_CALL, OP_CALLM, OP_NEW, OP_RET, OP_RETUNDEF,
    OP_THIS, OP_ARRAY, OP_OBJECT, OP_INITPROP, OP_INITELEM, OP_INITGET,
    OP_INITSET, OP_APPEND, OP_SPREAD, OP_CALLSPREAD,
    OP_THROW, OP_TRY, OP_ENDTRY,
    OP_FORIN, OP_FOROF, OP_ITERNEXT,
    OP_REGEXP, OP_TEMPLATE, OP_VOID, OP_DEBUG, OP_OPTCHAIN, OP_SETPROTO,
    OP_INITHIDDEN, OP_AWAIT, OP_INITGETELEM, OP_INITSETELEM,
    OP_COUNT
};

#endif
