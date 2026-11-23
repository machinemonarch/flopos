/* ===========================================================================
 * room.h -- the script room, as the kernel, browzer and the room itself
 * see it.  (Included by all three: keep it to plain numbers.)
 *
 * The room is 4 MB of its own, apart from browzer's 8 MB, mapped in 4 KB
 * pages so each part can be locked as it should be:
 *
 *   ROOM_BASE          the engine's code and constants: read-only
 *   then               its variables, then the heap the scripts use
 *   ROOM_GAP1          a page that is not there (the stack's floor)
 *   ROOM_STACK         256 KB of stack
 *   ROOM_GAP2          a page that is not there
 *   ROOM_MAIL          64 KB where tasks arrive and answers are left
 *
 * A task is given by browzer through the kernel (room_task in app.h);
 * the room answers it and stops.  While it works it may knock on the
 * door: the kernel copies the request to browzer, browzer answers, the
 * kernel copies the answer back.  Nothing else leaves the room.
 * ===========================================================================
 */
#ifndef ROOM_H
#define ROOM_H

#define ROOM_BASE   0x01800000u          /* 24 MB: above the apps' 12 MB      */
#define ROOM_SIZE   0x00700000u          /* 7 MB: a 32 MB machine's top megabyte left alone */
#define ROOM_END    (ROOM_BASE + ROOM_SIZE)
#define ROOM_MAIL_SIZE 0x10000u
#define ROOM_MAIL   (ROOM_END - ROOM_MAIL_SIZE)
#define ROOM_GAP2   (ROOM_MAIL - 0x1000u)
#define ROOM_STACK_SIZE 0x40000u
#define ROOM_STACK  (ROOM_GAP2 - ROOM_STACK_SIZE)
#define ROOM_GAP1   (ROOM_STACK - 0x1000u)
#define ROOM_VERSION 2

typedef struct {
    char magic[8];                       /* "FLOPROOM"                      */
    unsigned version;
    unsigned load_addr;                  /* ROOM_BASE                       */
    unsigned ro_end;                     /* code and constants end here (page aligned) */
    unsigned image_end;
    unsigned bss_end;
    unsigned entry;                      /* void room_main(unsigned len)    */
    unsigned guard;                      /* its stack canary, which the kernel sets */
} RoomHeader;

/* What the room asks the kernel: eax the number, ebx ecx edx esi. */
#define RS_DONE  1                       /* the answer is in the mail, ebx bytes */
#define RS_MS    2                       /* the clock                       */
#define RS_DOOR  3                       /* ebx request, ecx its length, edx answer, esi room for it */

/* Tasks: the first byte of the mail. */
#define T_INIT   1                       /* f64 the date now (ms since 1970) */
#define T_RUN    2                       /* u32 ms, u32 node, name\0, source (after any T_SRC pieces) */
#define T_CALL   3                       /* u32 ms, u32 handler id, event   */
#define T_TIMERS 4                       /* u32 ms                          */
#define T_CLEAR  5                       /* a new page: forget the old one  */

/* Answers: the first byte, then u32 the next timer (host ms, 0: none),
 * a byte of flags (1: an event's default was prevented), then a sentence
 * if it went wrong. */
#define A_OK     0
#define A_SYNTAX 1
#define A_THREW  2
#define A_TIME   3
#define A_ROOM   4

#endif
