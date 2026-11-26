// meter: the unit's own metronome accents every P beats instead of every 4 (open-field 0.12, base.pack 30, OS 1.7.3+0
// only). Step 1 of analysis/time-signature/FINDINGS.md: one place of the firmware, the metronome's beat function at
// 0x8f00ef54, reached through the pack's table (profile.json) and the hook in meter_hook.s. The count-in is not
// touched: it stays four beats. Compiled for the PC as it is, with OP1F_HOST (host/check.cpp).
//
// The firmware's side, as the study gives it: the beat function returns the beat number b in R0, or -1 when there is
// no beat to handle; its one caller accents when (b & 3) == 1 and reads b for nothing else. The hook runs the
// firmware's function once, unchanged, and hands its b to meter_map below, whose answer goes back to the caller:
// b itself when b < 0, else 1 on the first beat of a bar of P beats and 2 on any other, so that the caller's own test
// gives the accent. At 4/4 (P = 4) the caller decides exactly as with the stock function for every b.
//
// Threads. meter_map runs on the audio thread, once per audio block while the metronome runs: it reads the meter
// word once, writes five single words for the page and the log, calls nothing, logs nothing. The input function runs
// on the key thread (help, the knobs' turns and clicks): it writes counts only, and shows or closes the page. The
// frame function and `changed` run on the display thread: the frame function alone sets the meter word (one store),
// reads and writes the tape's value, paints, and writes the log's lines.
//
// The meter word: bits 0-7 the top number M (1 to 16), bits 8-15 the bottom (4 or 8), bits 16-23 the period P in
// clicks, bit 24 set when the click is on eighth notes (x/8 only). P is worked out here when the meter is set.
// Click on quarter notes: M for x/4; M / 2 for x/8 with M even (6/8 accents every three clicks); M for x/8 with M
// odd (7/8 on a quarter-note click can only be accented every two bars of 7/8). Click on eighth notes: M.
//
// Nothing is divided, nothing of a runtime is called, no float, no 64-bit number, no struct copy.
//
// Version 3 adds the tape (meter-3/NOTES.md; the firmware read 2026-11-24). Four places, each an eight-byte call of
// an on-chip routine taken with the form `call` and a room (meter_tape.s), each asking one small function below:
//   A 0x8fc83362  the tape grid's function: how many lines make a bar (the firmware's: the grid setting N)
//   B 0x8fc8338e  the same function: the dividend of the line spacing (the firmware's: 240.0, over bpm x N)
//   C 0x8fc9f19a  the snap unit's function: its dividend (the firmware's: 10584000.0); the unit is one grid line, and
//                 the keys that jump along the tape, the start of a recording and a drop all go by it
//   D 0x8c0c2ca2  winding the tape by hand: the dividend of the bar it stops at (the firmware's: 240.0, over bpm)
// Each function answers 0 for "the firmware's own" (then the stub changes nothing and the firmware's call runs as it
// came) or the number to use, from the constant table meter_tables.h. At 4/4 and 8/8 every answer is 0.
// Version 2 (the observation of places A, B and C) is gone: what it was to find out was read in the firmware.
//
// Version 4 adds two things (meter-4/NOTES.md):
//   - a click on eighth notes for the x/8 meters, with the accent on the first eighth of each bar. The hook and the
//     firmware's side are step 1's; meter_map alone works out the eighth notes, from the firmware's own phase of the
//     beat. The red knob on the page sets it back to quarter notes.
//   - the count-in in the meter: one bar of clicks (of the meter's clicks) in place of four. Three places: a hook at
//     the count-in's step function 0x8f01fd00 (a jump with a trampoline, the firmware's function run once through
//     it, then meter_countin); the address of the count-in's record that the metronome loads at 0x8f00f180 (form
//     `load`: it reads this pack's own record instead, which meter_countin keeps); and the load of the number the
//     count-in's popup prints, at 0x8fc5fc64 (a jump with a trampoline; meter_popup sets the number first).

#include "build_r6e_card.h"
#include "build_q_quiet.h"

typedef unsigned int u32;
typedef unsigned char u8;

extern "C" const Op1fCard op1f_card_meter OP1F_NAMED("op1f_card_meter");
#define CARD (&op1f_card_meter)

enum { KEY_HELP = 0, KNOBS = 4, KNOB_TOP = 0, KNOB_BOTTOM = 1, KNOB_CLICK = 3, CLICK0 = 54, PRESS0 = 61, CLICK_RESET = CLICK0 + 2 };
enum { FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36, TEMPO_SCREEN = 42 };
enum { TOP_MOST = 16, VERSION = 1, TEXT_ROOM = 96, PERIOD_FRAMES = 512, SAID_MOST = 24, CHANGES_SAID_MOST = 64, REST_MS = 600, RUNNING_MS = 400 };

static const char LOG_FILE[] = "op1f/meter.cpp";
static const char KEPT[] = "meter";
static const unsigned BLUE = OP1F_COLOUR(0x3c, 0x60, 0x87);         // the knobs' colours
static const unsigned OCHRE = OP1F_COLOUR(0x8a, 0x65, 0x3c);
static const unsigned GREY = OP1F_COLOUR(0xa2, 0xa4, 0xa6);
static const unsigned LIGHT_GREY = OP1F_COLOUR(0xcd, 0xd0, 0xc2);
static const unsigned DARK_GREY = OP1F_COLOUR(0x37, 0x3d, 0x42);
static const unsigned MID_GREY = OP1F_COLOUR(0x6a, 0x6e, 0x72);
static const unsigned LIGHT_BLUE = OP1F_COLOUR(0x7f, 0xb1, 0xe3);

// ---- the meter word
static inline u32 meter_period(u32 top, u32 bottom) { return (bottom == 8u && (top & 1u) == 0u) ? top >> 1 : top; }
enum { EIGHTHS = 1u << 24 };
static inline u32 meter_make(u32 top, u32 bottom, u32 eighths) {
    if (bottom == 8u && eighths) return top | bottom << 8 | top << 16 | (u32)EIGHTHS;
    return top | bottom << 8 | meter_period(top, bottom) << 16;
}
static inline u32 meter_eighths(u32 w) { return (w >> 24) & 1u; }
static inline u32 meter_top(u32 w) { return w & 0xffu; }
static inline u32 meter_bottom(u32 w) { return (w >> 8) & 0xffu; }
static inline u32 meter_p(u32 w) { return (w >> 16) & 0xffu; }

// (b - 1) mod p as mathematics has it, for 0 <= b and 1 <= p <= 16, without a divide: b = 0 gives p - 1.
// x = hi * 65536 + lo is first folded to hi * (65536 mod p) + lo, under 2^20, then 2^20 * p, 2^19 * p, ..., p are
// taken off where they fit: 21 compare-and-subtract steps with one comparand halved at each, no loop. The empty asm
// after each halving only hides d's value from the compiler, which would otherwise work out all 21 comparands at
// once and spill them into the address registers (I, B, M); it emits no instruction.
static inline u32 meter_place(int b, u32 p) {
    static const u8 R16[17] = { 0, 0, 0, 1, 0, 1, 4, 2, 0, 7, 6, 9, 4, 3, 2, 1, 0 };   // 65536 mod p
    if (b == 0) return p - 1u;
    u32 x = (u32)b - 1u;
    u32 y = (x >> 16) * (u32)R16[p] + (x & 0xffffu);
    u32 d = p << 20;
#define MT_STEP() if (y >= d) y -= d; d >>= 1; __asm__("" : "+r"(d))
    MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP();
    MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP();
    MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP(); MT_STEP();
#undef MT_STEP
    return y;
}

// ---- shared: single words
static volatile u32 meter_word = 4u | 4u << 8 | 4u << 16;          // the frame function's alone to write: 4/4
static volatile u32 map_calls, map_beats, map_accents, map_pos;     // meter_map's alone to write
static volatile int map_last = -1;
static volatile u32 keys, resets, ignored;                          // input's alone
static volatile int turned[2];                                      // input's alone: totals of the blue and second knobs
static volatile int turned_click;                                   // input's alone: the red knob's total
static volatile u32 told;                                           // changed's: one bit for each `how`

// ---- the firmware's words the click and the count-in read (OS 1.7.3+0; the pack's table holds checksums of this
// firmware, so under any other the loader leaves the pack out and none of these is ever read)
#ifdef OP1F_HOST
extern u32 fw_tape_pos, fw_beat_fixed;
extern unsigned short fw_block_count;
extern u8 fw_countin[20];
extern int fw_popup_beats;
#define FW_TAPE_POS (fw_tape_pos)
#define FW_BEAT_FIXED (fw_beat_fixed)
#define FW_BLOCK_COUNT (fw_block_count)
#define FW_COUNTIN (fw_countin)
#define FW_POPUP_BEATS (fw_popup_beats)
#else
#define FW_TAPE_POS (*(const volatile u32*)0x11905a18u)         // the tape's position, frames << 6 (the beat function reads it)
#define FW_BEAT_FIXED (*(const volatile u32*)0x84be1978u)       // a beat (a quarter note) in frames << 14
#define FW_BLOCK_COUNT (*(const volatile unsigned short*)0x84ad0708u)   // the audio blocks, counted by the firmware
#define FW_COUNTIN ((volatile u8*)0x84c680d4u)                  // the count-in's record (COUNTIN_ below)
#define FW_POPUP_BEATS (*(volatile int*)0x848d712cu)            // the beats left, as the count-in's popup last saw them
#endif

// The eighth-note click's own words, meter_map's alone (the audio thread).
static volatile u32 eighth_block, eighth_seen, eighth_half, eighth_at, eighth_due;

// Called by the hook (meter_hook.s) on the audio thread with the beat function's answer; its answer goes to the
// caller. Reads the meter word once; nothing else of the frame function's.
//
// Click on quarter notes (step 1): b itself when b < 0, else 1 on the first beat of a bar and 2 on any other.
//
// Click on eighth notes. The firmware's function answers b when the tape's position has come into another quarter
// note than at the call before: b is the quarter's number from the tape's start, 1 the first, so the quarter began
// at (b - 1) beats. That is a click, at the place (2 (b - 1)) mod M of the bar of M eighths. The eighth note between
// two quarters is due half a beat behind the quarter's own beginning: eighth_due, in frames, worked out at the
// quarter's click from b and the firmware's beat length (two 16-bit halves, nothing divided, no number over 32
// bits). When the tape's position has reached it, this function answers a click once, at the next place of the bar.
// So both clicks stand on the tape's own positions, as the firmware's quarter notes do.
// When the calls did not follow one another block by block (the tape stood still, or a count-in ran), the quarter
// the tape is in is not known until the firmware tells the next: no eighth is clicked before that.
extern "C" int meter_map(int b) OP1F_NAMED("meter_map");
extern "C" int meter_map(int b) {
    map_calls = map_calls + 1u;
    u32 w = meter_word;
    if (meter_eighths(w)) {
        u32 m = meter_top(w);
        if (m < 1u || m > (u32)TOP_MOST) m = 4u;
        u32 block = FW_BLOCK_COUNT;
        u32 gap = ((block - eighth_block) & 0xffffu) != 1u || !eighth_seen;   // (the first call ever follows no other)
        eighth_block = block;
        eighth_seen = 1;
        u32 pos = FW_TAPE_POS >> 6;                                 // frames
        if (b >= 0) {                                               // a quarter note begins
            u32 at = meter_place(b, m) << 1;                        // (2 (b - 1)) mod m: under 2 m
            if (at >= m) at -= m;
            u32 beat = FW_BEAT_FIXED;                               // frames << 14
            u32 n = b > 0 ? (u32)b - 1u : 0u;                       // quarters before this one (under 2^16)
            u32 began = ((n * (beat >> 16)) << 2) + ((n * (beat & 0xffffu)) >> 14);   // n beats in frames
            u32 due = began + (beat >> 15);
            eighth_due = due;
            eighth_half = pos >= due;                               // (come into the quarter's second half: its eighth is past)
            eighth_at = at;
            map_pos = at;
            map_last = b;
            map_beats = map_beats + 1u;
            if (at != 0u) return 2;
            map_accents = map_accents + 1u;
            return 1;
        }
        if (gap) { eighth_half = 1; return b; }                     // not the block after the last call
        if (eighth_half || pos < eighth_due) return b;
        eighth_half = 1;                                            // the eighth note between two quarters
        u32 at = eighth_at + 1u;
        if (at >= m) at = 0;
        eighth_at = at;
        map_pos = at;
        map_beats = map_beats + 1u;
        if (at != 0u) return 2;
        map_accents = map_accents + 1u;
        return 1;
    }
    if (b < 0) return b;
    u32 p = meter_p(w);
    if (p < 1u || p > (u32)TOP_MOST) p = 4u;
    u32 at = meter_place(b, p);
    map_pos = at;
    map_last = b;
    map_beats = map_beats + 1u;
    if (at != 0u) return 2;
    map_accents = map_accents + 1u;
    return 1;
}

// ---- the count-in (version 4)
//
// The firmware's side. The count-in's record (20 bytes): +0 running, +1 done, +4 the tape's position at its end, +8
// frames left, +0xc beats left, +0x10 "the beats left changed in this block". Its step function runs once a block:
// while the count-in does not run it sets beats left to -1 and frames left to four beats; while it runs it takes the
// block's frames off, sets beats left to frames left / a beat (3, 2, 1, 0) and the changed mark. The metronome, later
// in the same block, clicks when the mark is set, with the accent when (beats left & 3) is 0: the last click. The
// popup on the screen prints 4 - beats left: 1, 2, 3, 4.
//
// meter_countin runs behind the firmware's step function, every block, on the audio thread. For a count-in of
// `clicks` clicks, each `unit` frames long (a beat, or half a beat when the click is on eighth notes):
//   - while the count-in does not run, frames left is set to clicks x unit, so that the firmware counts that long;
//   - while it runs, our own clicks left is frames left / unit, and this pack's own record, which the metronome reads
//     in place of the firmware's (the place 0x8f00f180), gets: running, the changed mark for OUR clicks, and "beats
//     left" 0 at the last click and 1 before it, so that the accent stays where the firmware has it: on the last.
// At four quarter-note clicks (4/4 and every other meter of four clicks) the pack's record is a copy of the
// firmware's and frames left is not written. The firmware's own record is otherwise only read.
enum { COUNTIN_RUNNING = 0, COUNTIN_FRAMES = 8, COUNTIN_BEATS = 12, COUNTIN_CHANGED = 16, COUNTIN_BYTES = 20 };
struct CountIn { u8 running, done, pad0, pad1; u32 position; int frames; int beats; u32 changed; };
// In the initialised data (the table's `load` entry must name a place there): beats left -1, as the firmware has it
// while no count-in runs.
extern "C" volatile CountIn meter_countin_record OP1F_NAMED("meter_countin_record");
volatile CountIn meter_countin_record = { 0, 0, 0, 0, 0, 0, -1, 0 };
static volatile int countin_left = -1;                              // our clicks left; -1 while none runs
static volatile u32 countin_clicks = 4, countin_ours;               // of the count-in running or last run; 1: ours
static volatile u32 countin_calls, countin_runs, countin_said_clicks;

static inline int fw_int(const volatile u8* p) { return *(const volatile int*)p; }

extern "C" void meter_countin(void) OP1F_NAMED("meter_countin");
extern "C" void meter_countin(void) {
    countin_calls = countin_calls + 1u;
    volatile u8* rec = FW_COUNTIN;
    volatile CountIn* mine = &meter_countin_record;
    u32 w = meter_word;
    u32 clicks = meter_p(w);
    if (clicks < 1u || clicks > (u32)TOP_MOST) clicks = 4u;
    u32 beat = FW_BEAT_FIXED >> 14;                                 // a beat in frames, as the firmware takes it
    u32 unit = meter_eighths(w) ? beat >> 1 : beat;
    u32 running = rec[COUNTIN_RUNNING];
    if ((clicks == 4u && !meter_eighths(w)) || unit < 64u || unit > 0x40000u) {
        // the firmware's own count-in: our record is a copy of its
        if (!running) countin_ours = 0;
        if (!countin_ours) {
            int beats = fw_int(rec + COUNTIN_BEATS);
            mine->beats = beats;
            mine->changed = rec[COUNTIN_CHANGED];
            mine->running = (u8)running;
            countin_left = beats;
            countin_clicks = 4;
            return;
        }
        // (a count-in of ours still runs: the meter was changed while it ran; it goes on to its end as it began)
        clicks = countin_clicks;
    }
    if (!running) {
        *(volatile int*)(rec + COUNTIN_FRAMES) = (int)(clicks * unit);
        countin_clicks = clicks;
        countin_ours = 1;
        countin_left = -1;
        mine->changed = 0;
        mine->beats = -1;
        mine->running = 0;
        return;
    }
    if (countin_left < 0) countin_runs = countin_runs + 1u;
    int frames = fw_int(rec + COUNTIN_FRAMES);
    u32 rest = frames > 0 ? (u32)frames : 0u;
    // rest / unit, at most 31, without a divide: five compare-and-subtract steps
    u32 left = 0, d = unit << 4;
#define CI_STEP(bit) if (rest >= d) { rest -= d; left |= (bit); } d >>= 1; __asm__("" : "+r"(d))
    CI_STEP(16u); CI_STEP(8u); CI_STEP(4u); CI_STEP(2u); CI_STEP(1u);
#undef CI_STEP
    u32 changed = (int)left != countin_left && frames > 0;          // as the firmware: no mark in the block it ends in
    countin_left = (int)left;
    if (changed) countin_said_clicks = countin_said_clicks + 1u;
    mine->beats = left == 0u ? 0 : 1;
    mine->changed = changed;
    mine->running = 1;
}

// The count-in's popup: the firmware prints 4 - (the beats left as its popup last saw them). Called on the display
// thread just before the firmware loads that number: while a count-in of ours runs, the number is set so that the
// popup prints 1 at the first click and the number of clicks at the last.
extern "C" void meter_popup(void) OP1F_NAMED("meter_popup");
extern "C" void meter_popup(void) {
    if (!countin_ours) return;
    int left = countin_left;
    if (left < 0) return;
    FW_POPUP_BEATS = left + 4 - (int)countin_clicks;
}

extern "C" void meter_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("meter_changed");
extern "C" void meter_changed(unsigned where, unsigned which, unsigned how) {
    if (where != (unsigned)OP1F_KEEP_TAPE || which != 0 || how > 31u) return;
    told = told | (1u << how);
}

// ---- the tape (version 3): places A, B, C and D
//
// The firmware's side (OS 1.7.3+0). The tape screen draws its grid only while the tempo's mode is not "free". Its
// grid function reads the grid setting k (0..3, a word of the firmware's; shift + stop steps it), takes N = 1, 2, 4,
// 8 from a table, and draws a line every 240 / (bpm x N) seconds from the tape's start, the first line and every
// N-th after it as a bar line. The snap unit is 10584000 / (bpm x N) frames, one grid line. Winding by hand stops at
// every 240 / bpm seconds. The four functions below are called by the stubs of meter_tape.s on the thread that runs
// the firmware's code at the place (the display thread for A and B; the key, MIDI or display thread for C and D).
// Each reads the row of the meter (one load of a pointer into a constant table) and the firmware's grid setting,
// calls nothing, waits for nothing, divides nothing, has no loop, and writes only this pack's counts.
#include "meter_tables.h"
#ifdef OP1F_HOST
extern u32 fw_grid_setting;                                         // host/check.cpp
#define FW_GRID_SETTING (fw_grid_setting)
#else
#define FW_GRID_SETTING (*(const volatile u32*)0x848ce520u)         // the tape's grid setting, 0..3
#endif
enum { GRID_PLACES = 4, GRID_A = 0, GRID_B = 1, GRID_C = 2, GRID_D = 3, ROW_4_4 = 3 };

static const MeterRow* volatile meter_row = &METER_ROWS[ROW_4_4];   // the frame function's alone to write
static volatile u32 grid_d_latch;                                   // A's for B: this pass's dividend, 0 the firmware's
static volatile u32 grid_calls[GRID_PLACES], grid_ours[GRID_PLACES], grid_last[GRID_PLACES];

static inline const MeterRow* row_of(u32 top, u32 bottom) {
    u32 i = (bottom == 8u ? 16u : 0u) + top - 1u;
    return &METER_ROWS[i < (u32)METER_ROWS_N ? i : (u32)ROW_4_4];
}

// A: lines to a bar. Also settles B's dividend for this pass of the grid function, so that a meter changed between
// the two cannot give one pass the count of one meter and the spacing of another.
extern "C" u32 meter_grid_h(void) OP1F_NAMED("meter_grid_h");
extern "C" u32 meter_grid_h(void) {
    grid_calls[GRID_A] = grid_calls[GRID_A] + 1u;
    const MeterRow* row = meter_row;
    u32 k = FW_GRID_SETTING;
    if (row->stock || k > 3u) { grid_d_latch = 0; return 0; }
    u32 h = row->h[k];
    grid_d_latch = row->d[k];
    grid_ours[GRID_A] = grid_ours[GRID_A] + 1u;
    grid_last[GRID_A] = h;
    return h;
}
// B: the line spacing's dividend, as A settled it.
extern "C" u32 meter_grid_d(void) OP1F_NAMED("meter_grid_d");
extern "C" u32 meter_grid_d(void) {
    grid_calls[GRID_B] = grid_calls[GRID_B] + 1u;
    u32 d = grid_d_latch;
    if (!d) return 0;
    grid_ours[GRID_B] = grid_ours[GRID_B] + 1u;
    grid_last[GRID_B] = d;
    return d;
}
// C: the snap unit's dividend: one line of the grid as A and B draw it.
extern "C" u32 meter_snap_c(void) OP1F_NAMED("meter_snap_c");
extern "C" u32 meter_snap_c(void) {
    grid_calls[GRID_C] = grid_calls[GRID_C] + 1u;
    const MeterRow* row = meter_row;
    u32 k = FW_GRID_SETTING;
    if (row->stock || k > 3u) return 0;
    u32 c = row->c[k];
    grid_ours[GRID_C] = grid_ours[GRID_C] + 1u;
    grid_last[GRID_C] = c;
    return c;
}
// D: the dividend of the bar that winding by hand stops at.
extern "C" u32 meter_bar_d(void) OP1F_NAMED("meter_bar_d");
extern "C" u32 meter_bar_d(void) {
    grid_calls[GRID_D] = grid_calls[GRID_D] + 1u;
    const MeterRow* row = meter_row;
    if (row->stock) return 0;
    u32 d = row->bar;
    grid_ours[GRID_D] = grid_ours[GRID_D] + 1u;
    grid_last[GRID_D] = d;
    return d;
}

// Shift and help in any mode: the page opens, or closes when it is shown. On the page: the blue knob the top number,
// the second the bottom, a press of the third back to 4/4, the red knob the click of an x/8 meter (right: eighth
// notes, left: quarter notes); the third knob's turns and the other presses are taken and ignored.
extern "C" int meter_input(const Op1fInput* e) OP1F_NAMED("meter_input");
extern "C" int meter_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2) return 0;
    if (e->what == (u32)OP1F_DOWN_SHIFT && e->key == (u32)KEY_HELP) {
        keys = keys + 1u;
        if (e->mine) op1f_page_close(CARD);
        else op1f_page_show(CARD, 0, 0);                            // the base's blank page, painted each frame
        return 1;
    }
    if (e->what == (u32)OP1F_UP) return 1;                          // the up of a key whose down this pack took
    if (!e->mine) return 0;
    if (e->what == (u32)OP1F_TURN) {
        if (e->key >= (u32)KNOBS) return 0;
        if (e->key <= (u32)KNOB_BOTTOM) turned[e->key] = turned[e->key] + (e->turn >> 10);   // 1024 a step
        else if (e->key == (u32)KNOB_CLICK) turned_click = turned_click + (e->turn >> 10);
        else ignored = ignored + 1u;
        return 1;
    }
    if (e->key == (u32)CLICK_RESET && e->what == (u32)OP1F_DOWN) { resets = resets + 1u; return 1; }
    if (e->key >= (u32)PRESS0 && e->key < (u32)(PRESS0 + KNOBS)) return 1;
    if (e->key >= (u32)CLICK0 && e->key < (u32)(CLICK0 + KNOBS)) return 1;
    return 0;
}

// ---- the frame function's: texts, the tape's value, the page
static char* put(char* p, char* stop, const char* text) {
    while (*text && p < stop) *p++ = *text++;
    return p;
}
static char* put_number(char* p, char* stop, u32 v) {               // no division: powers of ten taken off
    static const u32 TENS[10] = { 1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u };
    u32 started = 0;
    for (u32 i = 0; i < 10u; ++i) {
        u32 digit = 0;
        while (v >= TENS[i]) { v -= TENS[i]; ++digit; }
        if (digit || started || i == 9u) {
            if (p < stop) *p++ = (char)('0' + digit);
            started = 1;
        }
    }
    return p;
}
static int text_width(const char* text, int scale) {                // as op1f_paint_text answers it
    int n = 0;
    while (text[n]) ++n;
    return n ? (6 * n - 1) * scale : 0;
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// A whole number member of the value's text: 1 and *out when "name": digits stands there, else 0.
static int member(const char* t, const char* name, int* out) {
    u32 n = 0;
    while (name[n]) ++n;
    for (const char* p = t; *p; ++p) {
        if (*p != '"') continue;
        const char* q = p + 1;
        u32 i = 0;
        while (i < n && q[i] == name[i]) ++i;
        if (i != n || q[n] != '"') {                                // another text: to its end
            while (*q && *q != '"') ++q;
            if (!*q) return 0;
            p = q;
            continue;
        }
        q += n + 1;
        while (is_space(*q)) ++q;
        if (*q != ':') return 0;
        ++q;
        while (is_space(*q)) ++q;
        int v = 0, digits = 0;
        while (*q >= '0' && *q <= '9' && digits < 6) { v = v * 10 + (*q - '0'); ++q; ++digits; }
        if (!digits) return 0;
        *out = v;
        return 1;
    }
    return 0;
}

// The tape's value: {"bottom":8,"top":7,"version":1}, members in any order. 1 with the two set when it is this form.
// "click" (version 4 of the pack; the value's own version stays 1, and an older pack reads over it): 4 the click of an
// x/8 meter on quarter notes, 8 or no such member on eighth notes.
static int read_kept(const char* t, u32* top, u32* bottom, u32* eighths) {
    int tp = 0, bt = 0, v = 0, ck = 8;
    if (!member(t, "top", &tp) || !member(t, "bottom", &bt) || !member(t, "version", &v)) return 0;
    if (v != VERSION || tp < 1 || tp > (int)TOP_MOST || (bt != 4 && bt != 8)) return 0;
    if (member(t, "click", &ck) && ck != 4 && ck != 8) return 0;
    *top = (u32)tp;
    *bottom = (u32)bt;
    *eighths = ck == 8;
    return 1;
}
static void write_kept(char* text, u32 top, u32 bottom, u32 eighths) {
    char* stop = text + TEXT_ROOM - 1;
    char* p = put(text, stop, "{\"bottom\":");
    p = put_number(p, stop, bottom);
    if (bottom == 8u) p = put(p, stop, eighths ? ",\"click\":8" : ",\"click\":4");
    p = put(p, stop, ",\"top\":");
    p = put_number(p, stop, top);
    p = put(p, stop, ",\"version\":1}");
    *p = 0;
}

static struct {
    u32 started, frames, said, said_changes, top, bottom, eighths, said_word, moved_ms, from_tape;
    u32 tape, load_waits, probed, held, held_word, keep_off, kept, damaged;
    int seen[2], seen_click;
    u32 resets_seen, calls_seen, calls_ms, countdown;
    char text[TEXT_ROOM];
    char line[TEXT_ROOM];
    QuietGate quiet;
} st;

static u32 word_now() { return meter_make(st.top, st.bottom, st.eighths); }

static void take(u32 top, u32 bottom, u32 eighths) { st.top = top; st.bottom = bottom; st.eighths = eighths; }

static void tape_loaded() {
    int got = op1f_value_get(CARD, OP1F_KEEP_TAPE, 0, KEPT, st.text, sizeof st.text);
    if (got == OP1F_NOT_NOW) return;                                // asked again at the next frame, nothing said
    st.load_waits = 0;
    st.tape = 1;
    u32 top = 4, bottom = 4, eighths = 1;
    if (got >= 0 && read_kept(st.text, &top, &bottom, &eighths)) {
        st.from_tape = 1;
    } else {
        st.from_tape = 0;                                           // a tape with none of ours (new, or from before): 4/4
        top = 4; bottom = 4; eighths = 1;
        if (got != OP1F_NO_FILE) {
            st.damaged = st.damaged + 1u;
            if (st.said < (u32)SAID_MOST) {
                ++st.said;
                op1f_log(2, LOG_FILE, 2, "meter: the tape's %s is not this form (answer %d): 4/4", KEPT, got);
            }
        }
    }
    take(top, bottom, eighths);
    st.held = 1;                                                    // what the tape holds, or 4/4 for none: nothing to write
    st.held_word = word_now();
}

// The start: L7 tells no load for the tape at the start (OP1F_KEPT_START is the sounds' alone), so the tape's value is
// read once, as maqam does. Only a value of this form is taken; anything else, "no value" too, waits for a load to be
// told, so that nothing is set over a tape's own value before it has been read (the docs do not say whether "no value"
// can come before the base has read the tape). Until then the meter shows and sounds, but is not kept.
static void tape_at_start() {
    int got = op1f_value_get(CARD, OP1F_KEEP_TAPE, 0, KEPT, st.text, sizeof st.text);
    if (got == OP1F_NOT_NOW) return;
    st.probed = 1;
    u32 top = 4, bottom = 4, eighths = 1;
    if (got < 0 || !read_kept(st.text, &top, &bottom, &eighths)) return;
    st.from_tape = 1;
    take(top, bottom, eighths);
    st.tape = 1;
    st.held = 1;
    st.held_word = word_now();
}

static void tape_keep() {
    if (!st.tape || st.load_waits || st.keep_off) return;
    if (st.held && st.held_word == word_now()) return;
    write_kept(st.text, st.top, st.bottom, st.eighths);
    int answer = op1f_value_set(CARD, OP1F_KEEP_TAPE, 0, KEPT, st.text);
    if (answer == OP1F_NOT_NOW) return;                             // a load not yet told: set at the next frame
    if (answer != 0) {
        st.keep_off = 1;
        op1f_log(2, LOG_FILE, 3, "meter: the base does not keep %s with the tape (answer %d): the meter is not kept", KEPT, answer);
        return;
    }
    st.kept = st.kept + 1u;
    st.held = 1;
    st.held_word = word_now();
}

static u32 clamped(u32 value, int steps, u32 least, u32 most) {
    int v = (int)value + steps;
    return v < (int)least ? least : v > (int)most ? most : (u32)v;
}

// "7/8"
static char* put_meter(char* p, char* stop, u32 w) {
    p = put_number(p, stop, meter_top(w));
    p = put(p, stop, "/");
    return put_number(p, stop, meter_bottom(w));
}
// "accent every 3 beats", "accent on every beat"; "accent every 7 eighth notes"
static char* put_period(char* p, char* stop, u32 w) {
    u32 per = meter_p(w);
    if (per == 1u) return put(p, stop, meter_eighths(w) ? "accent on every eighth note" : "accent on every beat");
    p = put(p, stop, "accent every ");
    p = put_number(p, stop, per);
    return put(p, stop, meter_eighths(w) ? " eighth notes" : " beats");
}
static const char* x8_note(u32 w) {
    if (meter_bottom(w) != 8u) return "";
    if (meter_eighths(w)) return "the click is on eighth notes (red knob left: on quarter notes)";
    if (meter_top(w) & 1u) return "the click is on quarter notes: an odd x/8 is accented every two bars";
    return "the click is on quarter notes: a bar of x/8 is half as many clicks";
}

static void paint_centre(void* canvas, int x, int y, const char* text, unsigned colour, int scale) {
    op1f_paint_text(canvas, x - (text_width(text, scale) >> 1), y, text, colour, scale);
}

static void paint_page(void* canvas, u32 w, u32 running) {
    char* stop = st.line + TEXT_ROOM - 1;
    char* p = put_meter(st.line, stop, w); *p = 0;
    paint_centre(canvas, 240, 12, st.line, LIGHT_GREY, 8);
    p = put_period(st.line, stop, w); *p = 0;
    paint_centre(canvas, 240, 80, st.line, OCHRE, 2);
    paint_centre(canvas, 240, 102, x8_note(w), MID_GREY, 1);
    // the bar: P dots, the first marked, the beat the click is on lit while the metronome runs
    u32 per = meter_p(w);
    int pitch = per > 1u ? 28 : 0;
    if (per > 14u) pitch = 26;
    int x0 = 240 - (int)((per - 1u) * (u32)pitch >> 1);
    u32 lit = running ? map_pos : 0xffu;
    for (u32 i = 0; i < per; ++i) {
        int x = x0 + (int)i * pitch;
        if (i == 0u) op1f_paint_box(canvas, x - 10, 124, 20, 20, OCHRE);
        unsigned c = i == lit ? LIGHT_BLUE : (i == 0u ? DARK_GREY : MID_GREY);
        op1f_paint_box(canvas, x - 7, 127, 14, 14, c);
    }
    if (running) {
        p = put(st.line, stop, meter_eighths(w) ? "eighth " : "beat ");
        p = put_number(p, stop, map_pos + 1u);
        p = put(p, stop, " of ");
        p = put_number(p, stop, per);
        *p = 0;
    } else {
        p = put(st.line, stop, map_beats ? "metronome stopped" : "no beat seen yet: start the metronome"); *p = 0;
    }
    paint_centre(canvas, 240, 156, st.line, running ? BLUE : MID_GREY, 1);
    paint_centre(canvas, 240, 176, "the tape's bar lines, snap and jumps and the count-in follow the meter", MID_GREY, 1);
    paint_centre(canvas, 240, 200, "blue: top  second: 4 or 8  red: x/8 click  press third: 4/4  shift+help: close", GREY, 1);
}

// Over the unit's own tempo screen (screen 42, docs/base.md): the meter small, top right.
static void paint_tempo(void* canvas, u32 w) {
    char* stop = st.line + TEXT_ROOM - 1;
    char* p = put_meter(st.line, stop, w); *p = 0;
    op1f_paint_text(canvas, 472 - text_width(st.line, 2), 6, st.line, OCHRE, 2);
}

extern "C" void meter_frame(const Op1fFrame* now) OP1F_NAMED("meter_frame");
extern "C" void meter_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    if (!st.started) {
        st.started = 1;
        take(4, 4, 1);
        st.said_word = word_now();
        st.seen[0] = turned[0]; st.seen[1] = turned[1]; st.seen_click = turned_click;
        st.resets_seen = resets;
        st.moved_ms = now->ms;
        op1f_log(3, LOG_FILE, 1, "meter: its place is the metronome's beat function 0x8f00ef54 (a jump, a trampoline of 8 bytes), "
                 "OS 1.7.3 only; the meter 4/4 until the tape's is read, an accent every 4 beats");
        op1f_log(3, LOG_FILE, 8, "meter: version 4: the tape follows the meter at four places: A 8fc83362 lines to a bar, B 8fc8338e line "
                 "spacing, C 8fc9f19a snap unit and jumps, D 8c0c2ca2 the bar winding stops at; 4/4 and 8/8: the firmware's own");
        op1f_log(3, LOG_FILE, 10, "meter: version 4: the count-in is one bar of clicks: its step 8f01fd00, the metronome's record 8f00f180, "
                 "the popup's number 8fc5fc64; four quarter-note clicks: the firmware's own. An x/8 meter clicks on eighth notes");
    }
    u32 t = told;                                                   // changed runs on this thread, never inside this call
    told = 0;
    if (t & ((1u << OP1F_KEPT_LOADED) | (1u << OP1F_KEPT_MADE))) st.load_waits = 1;
    if (st.load_waits && !st.keep_off) tape_loaded();
    else if (!st.tape && !st.probed && !st.keep_off) tape_at_start();
    int top_steps = turned[0] - st.seen[0], bottom_steps = turned[1] - st.seen[1];
    st.seen[0] += top_steps; st.seen[1] += bottom_steps;
    if (top_steps) st.top = clamped(st.top, top_steps, 1u, (u32)TOP_MOST);
    if (bottom_steps > 0) st.bottom = 8;
    if (bottom_steps < 0) st.bottom = 4;
    int click_steps = turned_click - st.seen_click;
    st.seen_click += click_steps;
    if (click_steps > 0) st.eighths = 1;                            // the red knob: right eighth notes, left quarter notes
    if (click_steps < 0) st.eighths = 0;
    u32 r = resets;
    if (r != st.resets_seen) { st.resets_seen = r; take(4, 4, 1); }
    u32 w = word_now();
    if (w != meter_word) {
        meter_row = row_of(st.top, st.bottom);                          // one store: the tape's four places read the row at once
        meter_word = w;                                                 // one store: the hook reads the three at once
        st.moved_ms = now->ms;
    }
    if (now->ms - st.moved_ms >= (u32)REST_MS) {
        if (w != st.said_word && st.said_changes < (u32)CHANGES_SAID_MOST) {
            ++st.said_changes;
            char* stop = st.line + TEXT_ROOM - 1;
            char* p = put_meter(st.line, stop, w); *p = 0;
            if (meter_eighths(w))
                op1f_log(3, LOG_FILE, 12, "meter: now %s, the click on eighth notes, an accent every %u eighth note(s)", st.line, meter_p(w));
            else
                op1f_log(3, LOG_FILE, 4, "meter: now %s, an accent every %u beat(s)%s", st.line, meter_p(w),
                         meter_bottom(w) == 8u && (meter_top(w) & 1u) ? " (every two bars of it on the quarter-note click)" : "");
            if (st.said_changes == (u32)CHANGES_SAID_MOST)
                op1f_log(3, LOG_FILE, 7, "meter: that was change line %u; later changes are not written here (the page still shows them)",
                         (u32)CHANGES_SAID_MOST);
        }
        st.said_word = w;
        tape_keep();
    }
    u32 calls = map_calls;
    if (calls != st.calls_seen) { st.calls_seen = calls; st.calls_ms = now->ms; }
    u32 running = calls && now->ms - st.calls_ms < (u32)RUNNING_MS;
    if (now->canvas) {
        if (now->page == CARD) paint_page(now->canvas, w, running);
        else if (now->page == 0 && now->screen == (u32)TEMPO_SCREEN) paint_tempo(now->canvas, w);
    }
    int why = quiet_popup(&st.quiet, now->popup != 0);
    int period = ++st.countdown >= (u32)PERIOD_FRAMES;
    if (!period && !why) return;
    if (period) {
        st.countdown = 0;
        u32 s = quiet_mix(quiet_start(), w);
        s = quiet_mix(s, map_beats);
        s = quiet_mix(s, map_accents);
        s = quiet_mix(s, st.kept + st.damaged + st.keep_off);
        s = quiet_mix(s, grid_last[GRID_A] ^ grid_last[GRID_B] ^ grid_last[GRID_C] ^ grid_last[GRID_D]);
        s = quiet_mix(s, (grid_ours[GRID_C] != 0u) | (grid_ours[GRID_D] != 0u) << 1 | (grid_calls[GRID_A] != 0u) << 2);
        s = quiet_mix(s, countin_runs);
        why |= quiet_period(&st.quiet, s, running);
    }
    if (!why) return;
    char* stop = st.line + TEXT_ROOM - 1;
    char* p = put_meter(st.line, stop, w); *p = 0;
    op1f_log(3, LOG_FILE, 5, "meter: %u call(s) of the beat function through the hook, %u beat(s), %u accent(s); the meter %s, every %u; "
             "the last beat %d at place %u; kept %u time(s), %u value(s) not this form; %u key(s), %u reset(s), %u turn(s) ignored",
             calls, map_beats, map_accents, st.line, meter_p(w), map_last, map_pos, st.kept, st.damaged, keys, resets, ignored);
    op1f_log(3, LOG_FILE, 9, "meter: the tape: grid setting %u; ours/calls, last answer: A %u/%u %u lines to a bar, B %u/%u %08x, C %u/%u %08x, D %u/%u %08x",
             FW_GRID_SETTING, grid_ours[GRID_A], grid_calls[GRID_A], grid_last[GRID_A], grid_ours[GRID_B], grid_calls[GRID_B], grid_last[GRID_B],
             grid_ours[GRID_C], grid_calls[GRID_C], grid_last[GRID_C], grid_ours[GRID_D], grid_calls[GRID_D], grid_last[GRID_D]);
    op1f_log(3, LOG_FILE, 11, "meter: the count-in: %u step(s) seen, %u count-in(s) of ours, %u click(s) of ours told, the last of %u click(s); "
             "the click on %s notes", countin_calls, countin_runs, countin_said_clicks, countin_clicks, meter_eighths(w) ? "eighth" : "quarter");
    if (st.quiet.silent_before)
        op1f_log(3, LOG_FILE, 6, "meter quiet: these lines after %u periods held (%s); %u printed, %u held", st.quiet.silent_before,
                 quiet_why(why), st.quiet.spoken, st.quiet.skipped);
}
