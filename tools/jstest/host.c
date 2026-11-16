/* ===========================================================================
 * host.c -- the script engine (room/) run on the PC that builds FLOPOS.
 *
 *     tools/jstest/run.sh              every test in tools/jstest/tests
 *     dist/jstest FILE.js ...          one or more, showing what they print
 *
 * A test prints what it checks with check(got, want, what); it passes if
 * it prints no FAIL and its last line is "done".  The engine here is the
 * same code as in the script room; only the clock, print and the door are
 * this program's.
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../room/js.h"
#include "../../room/engine.h"

static Vm s_vm;
static unsigned char s_heap[3 << 20];
static char s_out[1 << 20];
static unsigned s_outn;
static int s_echo;

u32 host_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u32)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int host_door(const u8 *in, u32 len, u8 *out, u32 cap) { (void)in; (void)len; (void)out; (void)cap; return -1; }

static Val print(Vm *vm, Val self, Val *args, int argc, u32 magic)
{
    int i;
    char b[4096];
    (void)self; (void)magic;
    for (i = 0; i < argc; i++) {
        Str *s = to_str(vm, args[i]);
        u32 n = str_to_utf8(s, b, sizeof b - 1);
        b[n] = 0;
        if (i && s_outn + 1 < sizeof s_out) s_out[s_outn++] = ' ';
        if (s_outn + n < sizeof s_out) { memcpy(s_out + s_outn, b, n); s_outn += n; }
        if (s_echo) printf("%s%s", i ? " " : "", b);
    }
    if (s_outn + 1 < sizeof s_out) s_out[s_outn++] = '\n';
    if (s_echo) printf("\n");
    return v_undef();
}

void host_init(Vm *vm)
{
    Obj *c = obj_new(vm, vm->object_proto, C_OBJECT);
    def_native(vm, vm->global, "print", print, 1, 0);
    def_val(vm, vm->global, "console", v_obj(c));
    def_native(vm, c, "log", print, 1, 0);
    def_native(vm, c, "error", print, 1, 0);
    def_native(vm, c, "warn", print, 1, 0);
}

static const char *PRELUDE =
    "function check(got, want, what) {"
    "  var g = typeof got === 'object' ? JSON.stringify(got) : String(got);"
    "  var w = typeof want === 'object' ? JSON.stringify(want) : String(want);"
    "  if (g !== w) print('FAIL ' + what + ': got ' + g + ', wanted ' + w);"
    "}\n";

static char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return 0; }
    b[*n] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    int i, failed = 0, quiet = 0, a0 = 1;
    extern int g_gc_stress;
    while (a0 < argc && argv[a0][0] == '-') {
        if (!strcmp(argv[a0], "-q")) quiet = 1;
        if (!strcmp(argv[a0], "-stress")) g_gc_stress = 1;
        a0++;
    }
    s_echo = !quiet;
    for (i = a0; i < argc; i++) {
        long n;
        char *src = slurp(argv[i], &n), err[300];
        int r, pass;
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!src) { printf("FAILED  %s: cannot read it\n", argv[i]); failed++; continue; }
        s_outn = 0;
        engine_init(&s_vm, s_heap, sizeof s_heap, 1700000000000.0);
        r = engine_run(&s_vm, PRELUDE, (u32)strlen(PRELUDE), "prelude", 1000, err, sizeof err);
        if (r) { printf("FAILED  the prelude: %s\n", err); return 1; }
        r = engine_run(&s_vm, src, (u32)n, name, 20000, err, sizeof err);
        /* timers and promises left to run */
        {
            int guard = 0;
            u32 next;
            while (!r && (next = engine_next_timer(&s_vm)) && guard++ < 1000) {
                while ((int)(host_ms() - next) < 0) { struct timespec t = { 0, 1000000 }; nanosleep(&t, 0); }
                if (engine_timers(&s_vm, 2000, err, sizeof err) < 0) { r = 9; break; }
            }
        }
        s_out[s_outn] = 0;
        pass = !r && !strstr(s_out, "FAIL") && s_outn >= 5 && !strcmp(s_out + s_outn - 5, "done\n");
        if (r && strstr(name, "expect-")) {
            /* a test that should be stopped: its first line says how */
            char want[200] = "";
            sscanf(src, "// expect: %199[^\n]", want);
            pass = want[0] && strstr(err, want) != 0;
            if (!pass && quiet) printf("        stopped with: %s\n", err);
        }
        if (quiet) {
            printf("%s  %s\n", pass ? "ok    " : "FAILED", name);
            if (!pass) {
                if (r && !strstr(name, "expect-")) printf("        stopped: %s\n", err);
                {
                    char *line = strtok(s_out, "\n");
                    int shown = 0;
                    while (line && shown < 12) { if (strstr(line, "FAIL")) { printf("        %s\n", line); shown++; } line = strtok(0, "\n"); }
                }
            }
        } else if (r) printf("[stopped: %s]\n", err);
        failed += !pass;
        free(src);
    }
    if (quiet && failed) printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
