/* The pack `meter`, version 4: the count-in's two routines (meter.cpp, "the count-in", says what the firmware does
 * and what the pack's functions do). Neither holds an instruction of the firmware.
 *
 * meter_countin_entry: the hook at the first instruction of the count-in's step function of OS 1.7.3+0 (0x8f01fd00),
 * in the manner of meter_hook.s. The pack's table has the loader put a JUMP.a to it over the function's first eight
 * bytes (three whole instructions, none relative to its own place) and copy those into meter_countin_room with a
 * JUMP.a back behind them. A call of the room is the firmware's function, run once, returning here.
 *
 *   What was read of the function (2026-11-24): one caller (the audio loop, 0x11a08024, once a block), one argument
 *   in R0 (the count-in's record), no answer the caller reads, and it never comes to be entered again before it has
 *   returned. At its first instructions it reads no register but R0, SP, FP and RETS.
 *
 *   1. The caller's return address is kept in a word of the pack's own zeroed data, meter_countin_back (one caller,
 *      one thread: written once and read once a call). That takes R3 and P1, which the function does not read.
 *      Nothing is pushed: SP and every word at and above it are the caller's at the call into the room.
 *   2. CALL meter_countin_room, by distance.
 *   3. Back here everything a call may have changed is pushed (ASTAT, R0-R3, P0-P2: 32 bytes), meter_countin() is
 *      called with twelve bytes of argument space, RETS is taken back from meter_countin_back, the registers are
 *      popped, and RTS goes back to the caller: with every register and the flags as the firmware's function left
 *      them, R0 too, and SP as at entry.
 *
 * meter_popup_load: at the eight-byte instruction of the count-in popup's draw function that loads the beats left
 * (0x8fc5fc64; the function then prints 4 minus that). Form `jump` with a room: the loader copies that one
 * instruction (an absolute load, not relative to its place) into meter_popup_load_room with a JUMP.a to the instruction
 * behind it. The routine pushes every register a call may change (36 bytes), calls meter_popup(), which sets the
 * number the instruction is about to load when a count-in of ours runs, pops them all and jumps to the room: the
 * firmware's own load runs with every register, the flags and the stack as they came.
 */
.text
.align 2

.global meter_countin_entry
.type meter_countin_entry, STT_FUNC
meter_countin_entry:
    R3 = RETS;                          /* the caller's return address */
    P1.H = meter_countin_back;
    P1.L = meter_countin_back;
    [P1] = R3;                          /* kept in the pack's own word, not on the stack */
    CALL meter_countin_room;            /* the firmware's step function, once, with R0, SP and the caller's stack as they came */
    [--SP] = ASTAT;                     /* what the function left, kept */
    [--SP] = R0;
    [--SP] = R1;
    [--SP] = R2;
    [--SP] = R3;
    [--SP] = P0;
    [--SP] = P1;
    [--SP] = P2;                        /* 32 bytes pushed */
    SP += -12;                          /* the argument space of the call */
    CALL meter_countin;
    SP += 12;
    P1.H = meter_countin_back;
    P1.L = meter_countin_back;
    R1 = [P1];
    RETS = R1;                          /* the caller's return address again */
    P2 = [SP++];
    P1 = [SP++];
    P0 = [SP++];
    R3 = [SP++];
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again */
    RTS;
.size meter_countin_entry, .-meter_countin_entry

.global meter_popup_load
.type meter_popup_load, STT_FUNC
meter_popup_load:
    [--SP] = ASTAT;
    [--SP] = RETS;
    [--SP] = P2;
    [--SP] = P1;
    [--SP] = P0;
    [--SP] = R3;
    [--SP] = R2;
    [--SP] = R1;
    [--SP] = R0;                        /* 36 bytes pushed: the entry SP is SP + 36 */
    SP += -12;                          /* the argument space of the call */
    CALL meter_popup;
    SP += 12;
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register as at entry */
    JUMP.L meter_popup_load_room;            /* the firmware's own load, then on behind it */
.size meter_popup_load, .-meter_popup_load

/* Zeroed data, which the loader clears when it loads the pack. Each room: the eight bytes its trampoline copies and
 * the eight of the jump back. meter_countin_back: the return address of the count-in step's caller. */
.section .bss
.balign 4
.global meter_countin_room
.type meter_countin_room, STT_OBJECT
meter_countin_room:
    .space 16
.size meter_countin_room, .-meter_countin_room
.global meter_popup_load_room
.type meter_popup_load_room, STT_OBJECT
meter_popup_load_room:
    .space 16
.size meter_popup_load_room, .-meter_popup_load_room
.type meter_countin_back, STT_OBJECT
meter_countin_back:
    .space 4
.size meter_countin_back, .-meter_countin_back
