/* engine.h -- the engine, as the room and the test program start it. */
#ifndef ENGINE_H
#define ENGINE_H

enum { ENGINE_OK, ENGINE_SYNTAX, ENGINE_THREW, ENGINE_TIME, ENGINE_ROOM };

void engine_init(Vm *vm, u8 *heap, u32 size, f64 now_base);
/* Run a script for at most ms; err gets a sentence when it is not OK. */
int  engine_run(Vm *vm, const char *src, u32 len, const char *name, u32 ms, char *err, u32 errcap);
int  engine_call(Vm *vm, Val fn, Val self, Val *args, int argc, u32 ms, char *err, u32 errcap);
void engine_begin(Vm *vm, u32 ms);
int  engine_finish(Vm *vm, char *err, u32 errcap);
u32  engine_next_timer(Vm *vm);          /* host_ms() of the next timer, 0 for none */
int  engine_timers(Vm *vm, u32 slice_ms, char *err, u32 errcap);

#endif
