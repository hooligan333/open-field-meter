/* The pack `meter` (meter.cpp says what it is for): its hook at the first instruction of the metronome's beat function
 * of OS 1.7.3+0 (0x8f00ef54). It holds no instruction of the firmware.
 *
 * The pack's table (meter_table.s, made from profile.json) has the loader put a JUMP.a to meter_beat_entry over the
 * function's first eight bytes, and copy those eight bytes (three whole instructions, none relative to its own place)
 * into meter_beat_room with a JUMP.a back behind them. A jump to the room is the firmware's function as it always
 * was; a call of the room is the same function returning to the caller of the room (the base's
 * op1f_engine_factory_entry, module/build_r6_engines.s, calls a room the same way; that is the tree's pattern).
 * Neither JUMP.a names a register: the loader's own jumps change none.
 *
 * What the study gives of the function: one caller; it answers the beat number b in R0, or -1 when there is no beat
 * to handle; the caller reads b only for the test for -1 and for (b & 3) == 1; it has a side effect of its own, so it
 * runs exactly once a call; its own arguments are not known. So the hook hands it exactly the registers and the
 * stack it was given:
 *
 *   1. Nothing is pushed before the call. The caller's return address (RETS) is kept in a word of the pack's own
 *      zeroed data, meter_back, not on the stack, so that SP and every word at and above it are the caller's at the
 *      call into the room. One word is enough: the function has one caller, on one thread (the audio thread, once a
 *      block while the metronome runs), and it is never entered again before it has returned (nothing it calls
 *      calls it), so meter_back is written once and read once a call, and no second call can stand between the two.
 *   2. Writing meter_back takes two registers: R3 (RETS's value) and P1 (meter_back's address). They are the only
 *      registers besides RETS that differ at the call. The function cannot depend on either at its first
 *      instruction: by the C convention a call may change R3 and P1, so its caller can leave nothing in them, and
 *      neither carries an argument (arguments come in R0-R2 and on the stack; P0 may carry the address of a returned
 *      structure, and is left alone) - docs/methods.md ("What is live there"), and examples/trampolines does the
 *      same with these two. ASTAT is not touched before the call: moves, loads and stores leave it as it is.
 *   3. CALL meter_beat_room, by distance (room and hook are in the same pack): RETS is our return point.
 *   4. Back here R0 is b. R1-R3, P0-P2 and ASTAT as the function left them are pushed (24 + 4 bytes) and kept, so that
 *      only R0 differs from the function's own return; R4-R7, P3-P5 and FP the function kept itself, and
 *      meter_map, compiled code, keeps them too (build/hook_proof.py checks its code).
 *   5. meter_map(b), with the twelve bytes of argument space every call must have (tools/stub_calls.py): its answer
 *      in R0.
 *   6. RETS is taken back from meter_back (through P1 and R1, before they are popped), the pushed registers are
 *      popped, and RTS goes back to the caller with SP as at entry and R0 the mapped value.
 *
 * The stack the hook takes at its deepest: 0 bytes at the call into the room (below which the function takes its own),
 * 40 at the call of meter_map (28 pushed and the twelve bytes), all below the entry SP. Nothing at or above the entry
 * SP is written by the hook.
 */
.text
.align 2

.global meter_beat_entry
.type meter_beat_entry, STT_FUNC
meter_beat_entry:
    R3 = RETS;                          /* the caller's return address */
    P1.H = meter_back;
    P1.L = meter_back;
    [P1] = R3;                          /* kept in the pack's own word, not on the stack */
    CALL meter_beat_room;               /* the firmware's beat function, once, with SP and the caller's stack as they came */
    [--SP] = ASTAT;                     /* what the function left, besides R0, kept */
    [--SP] = R1;
    [--SP] = R2;
    [--SP] = R3;
    [--SP] = P0;
    [--SP] = P1;
    [--SP] = P2;                        /* 28 bytes pushed */
    SP += -12;                          /* the argument space of the call */
    CALL meter_map;                     /* R0 = b in, the caller's value out */
    SP += 12;
    P1.H = meter_back;
    P1.L = meter_back;
    R1 = [P1];
    RETS = R1;                          /* the caller's return address again */
    P2 = [SP++];
    P1 = [SP++];
    P0 = [SP++];
    R3 = [SP++];
    R2 = [SP++];
    R1 = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again */
    RTS;
.size meter_beat_entry, .-meter_beat_entry

/* Zeroed data, which the loader clears when it loads the pack. The room: the eight bytes its trampoline copies and the
 * eight of the jump back, filled by the loader when it applies the table. meter_back: the caller's return address. */
.section .bss
.balign 4
.global meter_beat_room
.type meter_beat_room, STT_OBJECT
meter_beat_room:
    .space 16
.size meter_beat_room, .-meter_beat_room
.type meter_back, STT_OBJECT
meter_back:
    .space 4
.size meter_back, .-meter_back
