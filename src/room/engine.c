/* ===========================================================================
 * engine.c -- starting the engine, and the tasks it is given.
 *
 * Whoever runs the engine (room.c in the script room, or the test
 * program on the PC) gives it memory, then tasks: run this script, run
 * the timers that are due.  Each task has its own time; when it is up,
 * the task stops, and the next one starts afresh.
 * ===========================================================================
 */
#include "js.h"
#include "lex.h"
#include "engine.h"

void host_init(Vm *vm);
void module_init(Vm *vm);
void gen_init(Vm *vm);
u32  timers_next(Vm *vm);
int  timers_run(Vm *vm, u32 slice_ms);

void engine_init(Vm *vm, u8 *heap, u32 size, f64 now_base)
{
    memset(vm, 0, sizeof *vm);
    g_vm = vm;
    gc_init(heap, size);
    str_init_atoms();
    lex_init_keywords();
    vm->now_base = now_base;
    lib_init(vm);
    lib2_init(vm);
    gen_init(vm);
    re_init(vm);
    json_init(vm);
    module_init(vm);
    host_init(vm);
    vm->exception = v_undef();
}

void engine_begin(Vm *vm, u32 ms)
{
    vm->throwing = 0;
    vm->fatal = 0;
    vm->out_of_time = 0;
    vm->out_of_room = 0;
    vm->stop_why = 0;
    vm->door_ms = 0;
    vm->deadline = host_ms() + ms;
    vm->steps = 0;
    vm->sp = 0;
    vm->nframes = 0;
    vm->ntemps = 0;
    vm->door_count = 0;
}

/* What went wrong, in a sentence, for browzer's bottom line. */
static void why(Vm *vm, char *out, u32 cap)
{
    u32 n = 0;
    if (!cap) return;
    out[0] = 0;
    if (vm->stop_why) {
        const char *s = vm->stop_why;
        while (*s && n + 1 < cap) out[n++] = *s++;
        out[n] = 0;
        return;
    }
    if (vm->throwing) {
        Val e = vm->exception;
        Str *s;
        vm->throwing = 0;
        if (e.t == T_OBJ && e.u.o->cls == C_ERROR) {
            Val n2 = get_prop(vm, e, S_name), m = get_prop(vm, e, S_message);
            Str *a = n2.t == T_STR ? n2.u.s : atom("Error"), *b = m.t == T_STR ? m.u.s : S_empty;
            vm->throwing = 0;
            n = str_to_utf8(a, out, cap - 1);
            if (b->len && n + 2 < cap) { out[n++] = ':'; out[n++] = ' '; n += str_to_utf8(b, out + n, cap - 1 - n); }
            out[n] = 0;
            return;
        }
        s = to_str(vm, e);
        vm->throwing = 0;
        n = str_to_utf8(s ? s : S_empty, out, cap - 1);
        out[n] = 0;
        if (!n) { const char *t = "an exception"; while (*t && n + 1 < cap) out[n++] = *t++; out[n] = 0; }
    }
}

int engine_run(Vm *vm, const char *src, u32 len, const char *name, u32 ms, char *err, u32 errcap)
{
    Proto *p;
    engine_begin(vm, ms);
    if (err && errcap) err[0] = 0;
    p = js_compile(vm, src, len, name, err, errcap, 0);
    if (!p) return ENGINE_SYNTAX;
    vm_run_script(vm, p);
    if (vm->throwing || vm->fatal) {
        int r = vm->out_of_time ? ENGINE_TIME : vm->out_of_room ? ENGINE_ROOM : ENGINE_THREW;
        why(vm, err, errcap);
        vm->throwing = 0;
        return r;
    }
    vm_run_microtasks(vm);
    if (vm->fatal) { int r = vm->out_of_time ? ENGINE_TIME : ENGINE_ROOM; why(vm, err, errcap); return r; }
    gc_collect(vm);
    return ENGINE_OK;
}

/* Call a function the script made earlier (an event handler). */
int engine_call(Vm *vm, Val fn, Val self, Val *args, int argc, u32 ms, char *err, u32 errcap)
{
    engine_begin(vm, ms);
    if (err && errcap) err[0] = 0;
    vm_call(vm, fn, self, args, argc);
    if (vm->throwing || vm->fatal) {
        int r = vm->out_of_time ? ENGINE_TIME : vm->out_of_room ? ENGINE_ROOM : ENGINE_THREW;
        why(vm, err, errcap);
        vm->throwing = 0;
        return r;
    }
    vm_run_microtasks(vm);
    if (vm->fatal) { int r = vm->out_of_time ? ENGINE_TIME : ENGINE_ROOM; why(vm, err, errcap); return r; }
    return ENGINE_OK;
}

/* After a task's work: what became of it, and the promises it left. */
int engine_finish(Vm *vm, char *err, u32 errcap)
{
    if (vm->throwing || vm->fatal) {
        int r = vm->out_of_time ? ENGINE_TIME : vm->out_of_room ? ENGINE_ROOM : ENGINE_THREW;
        why(vm, err, errcap);
        vm->throwing = 0;
        return r;
    }
    vm_run_microtasks(vm);
    if (vm->fatal) { int r = vm->out_of_time ? ENGINE_TIME : ENGINE_ROOM; why(vm, err, errcap); return r; }
    return ENGINE_OK;
}

u32 engine_next_timer(Vm *vm) { return timers_next(vm); }

int engine_timers(Vm *vm, u32 slice_ms, char *err, u32 errcap)
{
    int n;
    engine_begin(vm, slice_ms);
    n = timers_run(vm, slice_ms);
    if (vm->fatal) { why(vm, err, errcap); return -1; }
    return n;
}
