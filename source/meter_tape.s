/* The pack `meter`, version 3: the four stubs by which the tape follows the meter (meter.cpp, "the tape", says what
 * each place is and what its function answers). None holds an instruction of the firmware.
 *
 *   A 0x8fc83362  the tape grid's function: the call that turns the grid setting N (a float) into an integer,
 *                 which the function then uses as the count of lines to a bar
 *   B 0x8fc8338e  later in the same function: the call of the divide for the line spacing, 240.0 / (bpm x N)
 *   C 0x8fc9f19a  the snap unit's function: the call of the same divide, 10584000.0 / (bpm x N)
 *   D 0x8c0c2ca2  winding the tape by hand: the call of the same divide, 240.0 / bpm (the bar it stops at)
 *
 * Each place is one eight-byte call of an on-chip routine. The pack's table (profile.json, form `call` with a room of
 * 16 bytes) has the loader copy that call into the place's room with a JUMP.a back to place+8 behind it, and write a
 * call of our stub over the place. So a stub is entered with RETS = place+8 and with every other register, the flags
 * and the stack as the firmware's call would have found them. Each stub
 *
 *   1. pushes ASTAT, RETS, P2, P1, P0, R3, R2, R1, R0 (36 bytes, all below the entry SP): every register it or its
 *      function may change (the C convention lets a callee change R0-R3, P0-P2, ASTAT and, by its CALL, RETS; the
 *      four functions keep every other register themselves, build/stub_proof.py checks this on their disassembly);
 *   2. calls its function, which takes nothing, with the twelve bytes of argument space every call must have;
 *   3. looks at the answer in R0. 0 means "the firmware's own": nothing is changed. Any other answer is written
 *      over the pushed R0;
 *   4. pops every register in the reverse order: SP, R1-R3, P0-P2, RETS and ASTAT are what they were at entry, and
 *      R0 is what it was at entry or the function's answer.
 *
 * Then B, C and D JUMP to the place's room by distance (JUMP.L names no register; room and stub are in the same
 * pack). There the firmware's own call of the divide runs with exactly the registers and the stack it would have
 * had, but, when the function answered, with our dividend in R0 in place of the firmware's constant. The routine
 * returns to room+8, whose JUMP.a goes to place+8.
 *
 * A does that too when its function answers 0. When the function answers a count, A returns it (RTS: RETS is
 * place+8, behind the place) and the on-chip routine is not called: the firmware's code behind place A reads only
 * R0 of what the routine leaves (it stores R0 as the count and loads every other register it uses afresh; read in
 * the firmware 2026-11-24, meter-3/NOTES.md).
 *
 * After a jump through a room, RETS holds room+8, not place+8 (compiled code does not read RETS after a call; the
 * base's stubs rely on the same). Nothing at or above the entry SP is written. Stack used below the entry SP:
 * 36 + 12 + what the function pushes.
 */
.text
.align 2

.global meter_tape_a
.type meter_tape_a, STT_FUNC
meter_tape_a:
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
    CALL meter_grid_h;
    SP += 12;
    CC = R0 == 0;
    IF CC JUMP .Lgrid_a_own;
    [SP] = R0;                          /* ours: the count, in place of the pushed R0 */
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register but R0 as at entry */
    RTS;                                /* behind place A, as the routine would return, R0 the count */
.Lgrid_a_own:
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register as at entry */
    JUMP.L meter_tape_a_room;           /* the firmware's own call, then back behind place A */
.size meter_tape_a, .-meter_tape_a

.global meter_tape_b
.type meter_tape_b, STT_FUNC
meter_tape_b:
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
    CALL meter_grid_d;
    SP += 12;
    CC = R0 == 0;
    IF CC JUMP .Lgrid_b_own;
    [SP] = R0;                          /* ours: the dividend, in place of the pushed R0 */
.Lgrid_b_own:
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register but R0 as at entry */
    JUMP.L meter_tape_b_room;           /* the firmware's own call of the divide, then back behind place B */
.size meter_tape_b, .-meter_tape_b

.global meter_tape_c
.type meter_tape_c, STT_FUNC
meter_tape_c:
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
    CALL meter_snap_c;
    SP += 12;
    CC = R0 == 0;
    IF CC JUMP .Lgrid_c_own;
    [SP] = R0;                          /* ours: the dividend, in place of the pushed R0 */
.Lgrid_c_own:
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register but R0 as at entry */
    JUMP.L meter_tape_c_room;           /* the firmware's own call of the divide, then back behind place C */
.size meter_tape_c, .-meter_tape_c

.global meter_tape_d
.type meter_tape_d, STT_FUNC
meter_tape_d:
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
    CALL meter_bar_d;
    SP += 12;
    CC = R0 == 0;
    IF CC JUMP .Lgrid_d_own;
    [SP] = R0;                          /* ours: the dividend, in place of the pushed R0 */
.Lgrid_d_own:
    R0 = [SP++];
    R1 = [SP++];
    R2 = [SP++];
    R3 = [SP++];
    P0 = [SP++];
    P1 = [SP++];
    P2 = [SP++];
    RETS = [SP++];
    ASTAT = [SP++];                     /* SP is the entry SP again; every register but R0 as at entry */
    JUMP.L meter_tape_d_room;           /* the firmware's own call of the divide, then back behind place D */
.size meter_tape_d, .-meter_tape_d

/* Zeroed data, which the loader clears when it loads the pack. Each room: the eight bytes its trampoline copies (the
 * firmware's call) and the eight of the jump back, filled by the loader when it applies the table. */
.section .bss
.balign 4
.global meter_tape_a_room
.type meter_tape_a_room, STT_OBJECT
meter_tape_a_room:
    .space 16
.size meter_tape_a_room, .-meter_tape_a_room
.global meter_tape_b_room
.type meter_tape_b_room, STT_OBJECT
meter_tape_b_room:
    .space 16
.size meter_tape_b_room, .-meter_tape_b_room
.global meter_tape_c_room
.type meter_tape_c_room, STT_OBJECT
meter_tape_c_room:
    .space 16
.size meter_tape_c_room, .-meter_tape_c_room
.global meter_tape_d_room
.type meter_tape_d_room, STT_OBJECT
meter_tape_d_room:
    .space 16
.size meter_tape_d_room, .-meter_tape_d_room
