// asf
#include "compiler.h"
#include "delay.h"
#include "gpio.h"
#include "intc.h"
#include "pm.h"
#include "print_funcs.h"
#include "spi.h"
#include "sysclk.h"

// system
#include "adc.h"
#include "events.h"
#include "font.h"
#include "hid.h"
#include "i2c.h"
#include "interrupts.h"
#include "init_common.h"
#include "init_teletype.h"
#include "region.h"
#include "screen.h"
#include "timers.h"
#include "util.h"

// board config
#include "conf_board.h"

// asf flash driver
#include "flashc.h"

// orca
#include "base.h"
#include "gbuffer.h"
#include "sim.h"
#include "vmio.h"

const char *git_version = "orca 0.1";

////////////////////////////////////////////////////////////////////////////////
// Flash storage

#define SCENE_SLOTS 32
#define SCENE_NAME_LEN 21  // fits one display line

// Doubles as a layout version: flash written by a firmware with a different
// scene layout (0x55 = 32x32 scenes) is treated as blank and reinitialised.
#define FIRSTRUN_KEY 0x64

#define GRID_W 64
#define GRID_H 64

typedef struct {
    Glyph grid[GRID_W * GRID_H];    // 4096 bytes
    char name[SCENE_NAME_LEN + 1];  // 22 bytes (null terminated)
    uint8_t metro_rate;              // 1 byte
    uint8_t swing;                  // 1 byte, 50-75 % (0 in older scenes = 50)
    uint8_t reserved[8];            // padding to 4128 bytes
} orca_scene_t;

// CV output calibration: the DAC code that gives exactly 1 V and 4 V on each
// output. Stored after the scenes, with its own validity check, so it
// doesn't depend on FIRSTRUN_KEY.
#define CAL_MAGIC 0x43414C31  // "CAL1"

typedef struct {
    uint16_t code_1v[4];
    uint16_t code_4v[4];
    uint32_t magic;  // CAL_MAGIC when valid; erased flash reads 0xFFFFFFFF
} cal_data_t;

typedef struct {
    uint8_t fresh;
    uint8_t last_scene;
    orca_scene_t scenes[SCENE_SLOTS];
    cal_data_t cal;
} nvram_data_t;

#if defined(__AVR32__)
static __attribute__((__section__(".flash_nvram"))) nvram_data_t f;
#else
static nvram_data_t f;
#endif

// Flash function prototypes (defined after grid variables)
static void flash_prepare(void);
static void flash_save_scene(uint8_t slot);
static void flash_save_scene_name(uint8_t slot, const char *name);
static void flash_load_scene(uint8_t slot);
static void flash_save_last_scene(uint8_t slot);
static uint8_t flash_is_scene_empty(uint8_t slot);
static void metro_restart(void);
static void set_playing(uint8_t on);
static void cal_enter(void);
static uint8_t ext_clock_active(void);

////////////////////////////////////////////////////////////////////////////////
// ORCA grid configuration

#define CHAR_W 6
#define CHAR_H 8

// Visible viewport size (characters that fit on screen). Text screens and
// the status line use CHAR_W cells; the grid packs tighter, GRID_CELL_W.
#define VIEW_W 21
#define GRID_CELL_W 5
#define GRID_COLS (128 / GRID_CELL_W)  // 25
#define VIEW_H 8

// Scroll margin: viewport scrolls when cursor is this close to the edge
#define SCROLL_MARGIN 2

// Grid buffers
static Glyph grid[GRID_H * GRID_W];
static Mark marks[GRID_H * GRID_W];
static Oevent_list oevent_list;
static Usz tick_number = 0;
static Usz random_seed = 42;
static uint8_t needs_remark = 1;  // grid changed: rebuild highlights

// Cursor position (in grid coordinates)
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;

// Selection: a rectangle from the cursor, sel_w x sel_h cells (1x1 = none)
static uint8_t sel_w = 1;
static uint8_t sel_h = 1;

// Append mode (Insert / Ctrl+I), like orca-c: typing moves the cursor right,
// Backspace moves left and clears, Space types a dot. Off = overwrite mode.
static uint8_t append_mode = 0;

// Viewport offset (top-left corner of visible area)
static uint8_t view_x = 0;
static uint8_t view_y = 0;

// Playback state
static uint8_t playing = 0;
static uint8_t metro_ms = 100;  // ms per tick (saved/loaded per scene)

// Swing, MPC 60 / MPC 1000 style: 50-75 % on 16ths (ORCA ticks). Each pair
// of ticks splits swing : 100 - swing, so odd ticks come late. 50 = straight,
// 66 = triplet feel. Set with the knob while the front button is held; saved
// per scene. Internal clock only.
#define SWING_MIN 50
#define SWING_MAX 75
#define SWING_STEPS (SWING_MAX - SWING_MIN + 1)

static uint8_t swing_pct = SWING_MIN;

// Status line: the bottom row shows the HUD (F1) and transient messages.
// A message takes over the bottom row for a moment even with the HUD off.
static uint8_t hud_on = 0;
static const char *grid_status = NULL;
static uint8_t grid_status_timer = 0;
#define GRID_STATUS_FRAMES 30  // ~2 sec at 15 Hz

static void set_status(const char *msg) {
    grid_status = msg;
    grid_status_timer = GRID_STATUS_FRAMES;
}

// n decimal digits of v, zero padded
static void put_digits(char *buf, uint32_t v, uint8_t n) {
    while (n--) {
        buf[n] = '0' + v % 10;
        v /= 10;
    }
}

// Grid rows on screen (the HUD takes the bottom one)
static uint8_t grid_rows(void) {
    return hud_on ? VIEW_H - 1 : VIEW_H;
}

////////////////////////////////////////////////////////////////////////////////
// Undo
//
// A log of the cells each edit changed (index + old glyph), grouped into
// steps, rather than whole-grid snapshots: a typed glyph costs one entry, a
// full-grid change at most 4096. The oldest steps are dropped to make room.
// All grid edits go through grid_write() between undo_begin()/undo_end().

#define UNDO_CELLS 6144
#define UNDO_STEPS 64

static uint16_t undo_idx[UNDO_CELLS];
static Glyph undo_old[UNDO_CELLS];
static uint16_t undo_cell_start = 0;  // oldest entry
static uint16_t undo_cell_count = 0;
static uint16_t undo_step_len[UNDO_STEPS];
static uint8_t undo_step_start = 0;   // oldest step
static uint8_t undo_step_count = 0;   // including the open step

static void undo_drop_oldest(void) {
    uint16_t len = undo_step_len[undo_step_start];
    undo_cell_start = (undo_cell_start + len) % UNDO_CELLS;
    undo_cell_count -= len;
    undo_step_start = (undo_step_start + 1) % UNDO_STEPS;
    undo_step_count--;
}

static void undo_begin(void) {
    if (undo_step_count == UNDO_STEPS) undo_drop_oldest();
    undo_step_len[(undo_step_start + undo_step_count) % UNDO_STEPS] = 0;
    undo_step_count++;
}

static void undo_end(void) {
    // an edit that changed nothing isn't worth a step
    if (undo_step_len[(undo_step_start + undo_step_count - 1) % UNDO_STEPS] == 0) {
        undo_step_count--;
    }
}

static void undo_clear(void) {
    undo_cell_start = undo_cell_count = 0;
    undo_step_start = undo_step_count = 0;
}

// Change one cell, remembering its old glyph in the open step
static void grid_write(uint16_t idx, Glyph g) {
    if (grid[idx] == g) return;
    // the open step is never dropped: it holds at most 4096 < UNDO_CELLS
    if (undo_cell_count == UNDO_CELLS) undo_drop_oldest();
    uint16_t at = (undo_cell_start + undo_cell_count) % UNDO_CELLS;
    undo_idx[at] = idx;
    undo_old[at] = grid[idx];
    undo_cell_count++;
    undo_step_len[(undo_step_start + undo_step_count - 1) % UNDO_STEPS]++;
    grid[idx] = g;
    needs_remark = 1;
}

static void undo_pop(void) {
    if (undo_step_count == 0) {
        set_status("NOTHING TO UNDO");
        return;
    }
    undo_step_count--;
    uint16_t len = undo_step_len[(undo_step_start + undo_step_count) % UNDO_STEPS];
    while (len--) {
        undo_cell_count--;
        uint16_t at = (undo_cell_start + undo_cell_count) % UNDO_CELLS;
        grid[undo_idx[at]] = undo_old[at];
    }
    needs_remark = 1;
    set_status("UNDO");
}

// Playback rewrites the grid. Remember it when play starts; when play stops,
// the cells that changed become one undo step.
static Glyph play_start_grid[GRID_H * GRID_W];

static void undo_play_started(void) {
    memcpy(play_start_grid, grid, sizeof(grid));
}

static void undo_play_stopped(void) {
    undo_begin();
    for (uint16_t i = 0; i < GRID_W * GRID_H; i++) {
        if (grid[i] != play_start_grid[i]) {
            Glyph now = grid[i];
            grid[i] = play_start_grid[i];
            grid_write(i, now);
        }
    }
    undo_end();
}

// UI mode
typedef enum {
    MODE_GRID,     // normal ORCA grid editing
    MODE_BROWSER,  // scene browser (select/load/save/rename)
    MODE_CAL,      // CV output calibration
} ui_mode_t;

static ui_mode_t ui_mode = MODE_GRID;

// Scene browser state
static uint8_t browser_slot = 0;     // currently highlighted slot
static uint8_t browser_scroll = 0;   // first visible slot in list
static uint8_t browser_editing = 0;  // 1 = editing scene name
static char browser_name[SCENE_NAME_LEN + 1]; // name edit buffer
static uint8_t browser_name_cursor = 0;       // cursor within name
static const char *browser_status = NULL;     // transient status message
static uint8_t browser_status_timer = 0;      // frames remaining for status
#define BROWSER_STATUS_FRAMES 45              // ~3 sec at 15 Hz refresh

////////////////////////////////////////////////////////////////////////////////
// Flash function definitions

// Written field by field: a scene is too big for a blank copy on the stack
static void flash_clear_scene(uint8_t slot) {
    orca_scene_t *s = &f.scenes[slot];
    flashc_memset8((void *)s->grid, '.', sizeof(s->grid), true);
    flashc_memset8((void *)s->name, 0, sizeof(s->name), true);
    flashc_memset8((void *)&s->metro_rate, 100, 1, true);
    flashc_memset8((void *)&s->swing, SWING_MIN, 1, true);
    flashc_memset8((void *)s->reserved, 0, sizeof(s->reserved), true);
}

static void flash_prepare(void) {
    if (f.fresh == FIRSTRUN_KEY) return;

    print_dbg("\r\nfirst run: clearing flash");

    for (uint8_t i = 0; i < SCENE_SLOTS; i++) {
        flash_clear_scene(i);
    }

    flashc_memset8((void *)&f.last_scene, 0, 1, true);
    flashc_memset8((void *)&f.fresh, FIRSTRUN_KEY, 1, true);
}

static void flash_save_scene(uint8_t slot) {
    if (slot >= SCENE_SLOTS) return;
    flashc_memcpy((void *)&f.scenes[slot].grid, grid, sizeof(grid), true);
    flashc_memset8((void *)&f.scenes[slot].metro_rate, metro_ms, 1, true);
    flashc_memset8((void *)&f.scenes[slot].swing, swing_pct, 1, true);
}

static void flash_save_scene_name(uint8_t slot, const char *name) {
    if (slot >= SCENE_SLOTS) return;
    flashc_memcpy((void *)&f.scenes[slot].name, name, SCENE_NAME_LEN + 1, true);
}

static void flash_load_scene(uint8_t slot) {
    if (slot >= SCENE_SLOTS) return;
    undo_begin();
    for (uint16_t i = 0; i < GRID_W * GRID_H; i++) {
        grid_write(i, f.scenes[slot].grid[i]);
    }
    undo_end();
    needs_remark = 1;
    tick_number = 0;
    uint8_t rate = f.scenes[slot].metro_rate;
    if (rate >= 20) metro_ms = rate;
    uint8_t sw = f.scenes[slot].swing;
    swing_pct = (sw >= SWING_MIN && sw <= SWING_MAX) ? sw : SWING_MIN;
}

static void flash_save_last_scene(uint8_t slot) {
    flashc_memset8((void *)&f.last_scene, slot, 1, true);
}

static uint8_t flash_is_scene_empty(uint8_t slot) {
    const Glyph *g = f.scenes[slot].grid;
    for (uint16_t i = 0; i < GRID_W * GRID_H; i++) {
        if (g[i] != '.') return 0;
    }
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Screen

region line[8] = { { .w = 128, .h = 8, .x = 0, .y = 0 },
                   { .w = 128, .h = 8, .x = 0, .y = 8 },
                   { .w = 128, .h = 8, .x = 0, .y = 16 },
                   { .w = 128, .h = 8, .x = 0, .y = 24 },
                   { .w = 128, .h = 8, .x = 0, .y = 32 },
                   { .w = 128, .h = 8, .x = 0, .y = 40 },
                   { .w = 128, .h = 8, .x = 0, .y = 48 },
                   { .w = 128, .h = 8, .x = 0, .y = 56 } };

// Software timers
static softTimer_t refreshTimer = { .next = NULL, .prev = NULL };
static softTimer_t keyTimer = { .next = NULL, .prev = NULL };
static softTimer_t metroTimer = { .next = NULL, .prev = NULL };
static softTimer_t hidTimer = { .next = NULL, .prev = NULL };
static softTimer_t gateTimer = { .next = NULL, .prev = NULL };

////////////////////////////////////////////////////////////////////////////////
// DAC output
//
// The teletype has two daisy-chained 12-bit DACs.
// Each SPI transaction sends 6 bytes (two 3-byte DAC commands).
// Command byte 0x31 = write+update channel A, 0x38 = write+update channel B.
// First transaction: DAC outputs 2 and 0 (chip A, channels A and B via daisy)
// Second transaction: DAC outputs 3 and 1
//
// AE Modular (Telex): DAC codes 0-4095 span 0-5 V, so 1 V/oct puts
// 60 semitones (C0-C5) across the full code range.

#define DAC_MAX 4095
#define DAC_SEMITONES 60

static uint16_t dac_values[4] = {0, 0, 0, 0};  // 12-bit DAC codes

// Note number (octave * 12 + note) -> DAC code at 1 V/oct, clamped at 5 V
static uint16_t note_to_dac(uint16_t note_num) {
    uint32_t code = ((uint32_t)note_num * (DAC_MAX + 1) + DAC_SEMITONES / 2)
                    / DAC_SEMITONES;
    return code > DAC_MAX ? DAC_MAX : (uint16_t)code;
}

static void dac_update(void) {
    const uint16_t *out = dac_values;

    // First chip select: channels 2 and 0
    spi_selectChip(DAC_SPI, DAC_SPI_NPCS);
    spi_write(DAC_SPI, 0x31);
    spi_write(DAC_SPI, out[2] >> 4);
    spi_write(DAC_SPI, out[2] << 4);
    spi_write(DAC_SPI, 0x31);
    spi_write(DAC_SPI, out[0] >> 4);
    spi_write(DAC_SPI, out[0] << 4);
    spi_unselectChip(DAC_SPI, DAC_SPI_NPCS);

    // Second chip select: channels 3 and 1
    spi_selectChip(DAC_SPI, DAC_SPI_NPCS);
    spi_write(DAC_SPI, 0x38);
    spi_write(DAC_SPI, out[3] >> 4);
    spi_write(DAC_SPI, out[3] << 4);
    spi_write(DAC_SPI, 0x38);
    spi_write(DAC_SPI, out[1] >> 4);
    spi_write(DAC_SPI, out[1] << 4);
    spi_unselectChip(DAC_SPI, DAC_SPI_NPCS);
}

// Calibration maps an ideal code onto the line through each channel's
// measured 1 V and 4 V codes. In units of code*5 the ideal 1 V and 4 V
// codes (819.2 and 3276.8) are exact: 4096 and 16384.
#define CAL_IDEAL_1V 819
#define CAL_IDEAL_4V 3277
#define CAL_1V_X5 4096
#define CAL_SPAN_X5 12288  // 4 V - 1 V

static cal_data_t cal;

static void cal_reset_channel(uint8_t ch) {
    cal.code_1v[ch] = CAL_IDEAL_1V;
    cal.code_4v[ch] = CAL_IDEAL_4V;
}

static void cal_load(void) {
    if (f.cal.magic == CAL_MAGIC) {
        cal = f.cal;
    } else {
        for (uint8_t i = 0; i < 4; i++) cal_reset_channel(i);
        cal.magic = CAL_MAGIC;
    }
}

static void cal_save(void) {
    flashc_memcpy((void *)&f.cal, &cal, sizeof(cal), true);
}

static uint16_t cal_apply(uint8_t ch, uint16_t ideal) {
    int32_t span = (int32_t)cal.code_4v[ch] - cal.code_1v[ch];
    int32_t num = ((int32_t)ideal * 5 - CAL_1V_X5) * span;
    // round half away from zero
    int32_t adj = num >= 0 ? (num + CAL_SPAN_X5 / 2) / CAL_SPAN_X5
                           : -((-num + CAL_SPAN_X5 / 2) / CAL_SPAN_X5);
    int32_t out = (int32_t)cal.code_1v[ch] + adj;
    if (out < 0) out = 0;
    if (out > DAC_MAX) out = DAC_MAX;
    return (uint16_t)out;
}

// value: ideal 12-bit code (0-4095 = 0-5 V); calibration is applied here
static void dac_set(uint8_t channel, uint16_t value) {
    if (channel < 4) {
        dac_values[channel] = cal_apply(channel, value);
        dac_update();
    }
}

////////////////////////////////////////////////////////////////////////////////
// Inputs: CV IN and the PARAM knob (AD7923 channels 0 and 1, 12-bit).
// CV IN 0-5 V reads 0-4095.

#define ADC_POLL_MS 20
#define SCALE_COUNT 12
#define KNOB_HYST 40          // ADC counts past a zone edge before switching
#define CV_IN_HYST_X60 3072   // 0.75 semitone, in units of adc*60

static u16 adc[4];
static softTimer_t adcTimer = { .next = NULL, .prev = NULL };

// Scale quantizer, selected by the PARAM knob. Bit n = semitone n above C.
static const uint16_t scale_masks[SCALE_COUNT] = {
    0xfff,  // free
    0xab5,  // major
    0x5ad,  // minor
    0x6ad,  // dorian
    0x5ab,  // phrygian
    0xad5,  // lydian
    0x6b5,  // mixolydian
    0x4a9,  // pentatonic minor
    0x295,  // pentatonic major
    0x9ad,  // harmonic minor
    0x555,  // whole tone
    0x4e9,  // blues
};
static const char *const scale_names[SCALE_COUNT] = {
    "SCALE FREE", "SCALE MAJOR", "SCALE MINOR", "SCALE DORIAN",
    "SCALE PHRYGIAN", "SCALE LYDIAN", "SCALE MIXOLYDIAN", "SCALE PENT MINOR",
    "SCALE PENT MAJOR", "SCALE HARM MINOR", "SCALE WHOLE TONE", "SCALE BLUES",
};
static uint8_t scale_idx = 0;
static uint8_t scale_known = 0;  // first reading sets the scale silently

// Nearest note in the scale; on a tie, the lower one
static uint16_t quantize(uint16_t n) {
    uint16_t mask = scale_masks[scale_idx];
    for (uint8_t d = 0; d < 12; d++) {
        if (n >= d && ((mask >> ((n - d) % 12)) & 1)) return n - d;
        if ((mask >> ((n + d) % 12)) & 1) return n + d;
    }
    return n;
}

// Knob zone 0..count-1 for a reading, staying in the current zone until the
// knob is KNOB_HYST past its edge
static uint8_t knob_zone(uint16_t knob, uint8_t current, uint8_t count) {
    int16_t lo = (int16_t)(current * 4096 / count) - KNOB_HYST;
    int16_t hi = (int16_t)((current + 1) * 4096 / count) + KNOB_HYST;
    if (knob >= lo && knob < hi) return current;
    return (uint32_t)knob * count / 4096;
}

static void update_scale(uint16_t knob) {
    uint8_t idx = scale_known ? knob_zone(knob, scale_idx, SCALE_COUNT)
                              : (uint32_t)knob * SCALE_COUNT / 4096;
    if (scale_known && idx != scale_idx) set_status(scale_names[idx]);
    scale_idx = idx;
    scale_known = 1;
}

static void show_swing(void) {
    static char buf[] = "SWING 00%";
    put_digits(&buf[6], swing_pct, 2);
    set_status(buf);
}

static void update_swing(uint16_t knob) {
    uint8_t pct = SWING_MIN + knob_zone(knob, swing_pct - SWING_MIN, SWING_STEPS);
    if (pct != swing_pct) {
        swing_pct = pct;
        show_swing();
    }
}

// The knob sets the scale, or the swing while the front button is held.
// After the button changes, the knob only takes over once it's turned, so
// the other setting doesn't jump to wherever the knob happens to be.
#define KNOB_TAKEOVER 80  // ADC counts

static uint8_t front_held = 0;
static uint8_t knob_live = 1;
static uint16_t knob_ref = 0;

static void knob_mode_changed(void) {
    knob_live = 0;
    knob_ref = adc[1];
}

static void update_knob(uint16_t knob) {
    if (!knob_live) {
        int16_t moved = (int16_t)knob - (int16_t)knob_ref;
        if (moved < KNOB_TAKEOVER && moved > -KNOB_TAKEOVER) return;
        knob_live = 1;
    }
    if (front_held) update_swing(knob);
    else update_scale(knob);
}

// CV IN as a 1 V/oct note number 0-60 for the & operator, with hysteresis
// so a voltage between two semitones doesn't flicker
static void update_cv_in_note(uint16_t v) {
    int32_t x60 = (int32_t)v * 60;
    int32_t diff = x60 - (int32_t)orca_cv_in_note * 4096;
    if (diff > CV_IN_HYST_X60 || diff < -CV_IN_HYST_X60) {
        uint32_t n = (x60 + 2048) / 4096;
        orca_cv_in_note = n > DAC_SEMITONES ? DAC_SEMITONES : (U8)n;
    }
}

static void read_inputs(void) {
    adc_convert(&adc);
    update_cv_in_note(adc[0]);
    update_knob(adc[1]);
}

static void adcTimer_callback(void *o) {
    (void)o;
    event_t e = { .type = kEventPollADC, .data = 0 };
    event_post(&e);
}

////////////////////////////////////////////////////////////////////////////////
// ORCA grid init

static void grid_init(void) {
    memset(grid, '.', sizeof(grid));
    memset(marks, 0, sizeof(marks));
    oevent_list_init(&oevent_list);
    tick_number = 0;
    needs_remark = 1;
}

////////////////////////////////////////////////////////////////////////////////
// Render grid to screen

// Update viewport to keep cursor visible with scroll margin
static void update_viewport(void) {
    // Horizontal scrolling
    if (cursor_x < view_x + SCROLL_MARGIN) {
        view_x = (cursor_x >= SCROLL_MARGIN) ? cursor_x - SCROLL_MARGIN : 0;
    }
    if (cursor_x >= view_x + GRID_COLS - SCROLL_MARGIN) {
        view_x = cursor_x - GRID_COLS + SCROLL_MARGIN + 1;
        if (view_x > GRID_W - GRID_COLS) view_x = GRID_W - GRID_COLS;
    }

    // Vertical scrolling
    uint8_t rows = grid_rows();
    if (cursor_y < view_y + SCROLL_MARGIN) {
        view_y = (cursor_y >= SCROLL_MARGIN) ? cursor_y - SCROLL_MARGIN : 0;
    }
    if (cursor_y >= view_y + rows - SCROLL_MARGIN) {
        view_y = cursor_y - rows + SCROLL_MARGIN + 1;
    }
    if (view_y > GRID_H - rows) view_y = GRID_H - rows;
}

// Grey levels (0-15) for the grid
#define SHADE_DOT    0x1  // empty cell
#define SHADE_RULER  0x1  // + ruler marks
#define SHADE_LOCK   0x2  // inside a comment / locked
#define SHADE_INPUT  0xf  // read by an operator
#define SHADE_OP     0xf  // operator or value
#define SHADE_OUT_BG 0x3  // background of a written cell
#define SHADE_CURSOR 0xf  // cursor block
#define SHADE_SEL    0x8  // rest of a selection
#define SHADE_HUD    0x9  // status line text

// Like orca-c's rulers: empty cells on every Nth row and column show a +.
// 7 rows (not orca-c's 8) so the 8-row screen always shows a row of them.
#define RULER_SPACING_X 7
#define RULER_SPACING_Y 7

// 4 px wide versions of the font's 5 px glyphs, so they keep a 1 px gap in
// 5 px grid cells. Column bytes, bit n = row n, like font_data.
typedef struct {
    char ch;
    uint8_t data[4];
} narrow_glyph_t;

static const narrow_glyph_t narrow_glyphs[] = {
    {'#', { 0x24, 0x7e, 0x24, 0x7e }},
    {'&', { 0x34, 0x5a, 0x24, 0x50 }},
    {'M', { 0x7e, 0x0c, 0x0c, 0x7e }},
    {'W', { 0x7e, 0x30, 0x30, 0x7e }},
    {'m', { 0x7c, 0x08, 0x0c, 0x78 }},
    {'w', { 0x7c, 0x30, 0x30, 0x7c }},
};

// Draw one glyph into a full cell_w x CHAR_H cell at x = px: the background
// covers the whole cell (not just the glyph's columns, as font_glyph() does)
// and the glyph is centred in it. Glyphs as wide as the cell use their
// narrow version if there is one.
static void draw_cell(region *reg, uint8_t px, uint8_t cell_w, char ch,
                      uint8_t fg, uint8_t bg) {
    const glyph_t *gl = &font_data[ch - FONT_ASCII_OFFSET];
    const uint8_t *cols = &gl->data[gl->first];
    uint8_t w = FONT_CHARW - gl->first - gl->last;
    if (w >= cell_w) {
        for (uint8_t i = 0; i < sizeof(narrow_glyphs) / sizeof(narrow_glyphs[0]); i++) {
            if (narrow_glyphs[i].ch == ch) {
                cols = narrow_glyphs[i].data;
                w = 4;
                break;
            }
        }
    }
    uint8_t start = w < cell_w ? (cell_w - w) / 2 : 0;
    uint8_t *cell = reg->data + px;
    for (uint8_t x = 0; x < cell_w; x++) {
        uint8_t bits = (x >= start && x < start + w) ? cols[x - start] : 0;
        for (uint8_t y = 0; y < CHAR_H; y++) {
            cell[x + y * reg->w] = (bits & (1 << y)) ? fg : bg;
        }
    }
}

static void draw_text(region *reg, const char *str, uint8_t fg) {
    for (uint8_t i = 0; i < VIEW_W && str[i]; i++) {
        draw_cell(reg, i * CHAR_W, CHAR_W, str[i], fg, 0);
    }
}

static const char scale_abbr[SCALE_COUNT][4] = {
    "FRE", "MAJ", "MIN", "DOR", "PHR", "LYD",
    "MIX", "PNm", "PNM", "HRM", "WHL", "BLU",
};

// Bottom row: a transient message, or the HUD
//   "> 0123 150b 12,05 MAJ"  state (> play, E external clock, | paused;
//   A after it in append mode),
//   tick, tempo in BPM, cursor x,y, scale
static void render_status_line(void) {
    region *reg = &line[VIEW_H - 1];
    region_fill(reg, 0);
    if (grid_status_timer > 0) {
        grid_status_timer--;
        draw_text(reg, grid_status, SHADE_OP);
        return;
    }
    char buf[VIEW_W + 1] = "> 0000 000b 00,00 FRE";
    buf[0] = !playing ? '|' : ext_clock_active() ? 'E' : '>';
    if (append_mode) buf[1] = 'A';
    put_digits(&buf[2], tick_number % 10000, 4);
    put_digits(&buf[7], 15000 / metro_ms, 3);  // 4 ticks per beat
    put_digits(&buf[12], cursor_x, 2);
    put_digits(&buf[15], cursor_y, 2);
    memcpy(&buf[18], scale_abbr[scale_idx], 3);
    draw_text(reg, buf, SHADE_HUD);
}

static void render_grid(void) {
    update_viewport();

    for (uint8_t vrow = 0; vrow < grid_rows(); vrow++) {
        uint8_t grid_row = view_y + vrow;
        Glyph *src = grid + grid_row * GRID_W;
        Mark *msrc = marks + grid_row * GRID_W;

        region_fill(&line[vrow], 0);

        for (uint8_t vcol = 0; vcol < GRID_COLS; vcol++) {
            uint8_t grid_col = view_x + vcol;
            char ch = src[grid_col];
            uint8_t fg = SHADE_OP;
            uint8_t bg = 0;
            if (ch == '.') {
                fg = SHADE_DOT;
                if (grid_row % RULER_SPACING_Y == 0 && grid_col % RULER_SPACING_X == 0) {
                    ch = '+';
                    fg = SHADE_RULER;
                }
            }

            // Mark coloring
            Mark m = msrc[grid_col];
            if (m & Mark_flag_output) {
                fg = SHADE_OP;
                bg = SHADE_OUT_BG;
            } else if (m & Mark_flag_input) {
                fg = SHADE_INPUT;
            } else if ((m & Mark_flag_lock) || src[grid_col] == '#') {
                // comments, including the opening # (the closing one is
                // already locked)
                fg = SHADE_LOCK;
            }

            // Cursor: solid block, glyph cut out of it. The rest of a
            // selection is a dimmer block.
            uint8_t is_cursor = grid_row == cursor_y && grid_col == cursor_x;
            uint8_t in_sel = grid_row >= cursor_y && grid_row < cursor_y + sel_h &&
                             grid_col >= cursor_x && grid_col < cursor_x + sel_w;
            if (in_sel) {
                fg = 0;
                bg = is_cursor ? SHADE_CURSOR : SHADE_SEL;
            }

            draw_cell(&line[vrow], vcol * GRID_CELL_W, GRID_CELL_W, ch, fg, bg);
        }
    }

    if (hud_on || grid_status_timer > 0) render_status_line();
}

static void draw_all(void) {
    for (int i = 0; i < 8; i++) {
        region_draw(&line[i]);
    }
}

////////////////////////////////////////////////////////////////////////////////
// HID key codes (from usb_protocol_hid.h)
#include "usb_protocol_hid.h"
#define HID_SPACE  0x2C
#define HID_BKSP   0x2A
#define HID_ESC    0x29

// Modifier bit positions in HID report byte 0
#define MOD_LCTRL  (1 << 0)
#define MOD_LSHIFT (1 << 1)
#define MOD_LALT   (1 << 2)
#define MOD_RCTRL  (1 << 4)
#define MOD_RSHIFT (1 << 5)
#define MOD_CTRL   (MOD_LCTRL | MOD_RCTRL)
#define MOD_SHIFT  (MOD_LSHIFT | MOD_RSHIFT)

////////////////////////////////////////////////////////////////////////////////
// Scene browser rendering

static void render_browser(void) {
    // 8 lines visible. Line 0 = header, lines 1-7 = scene slots
    #define BROWSER_VISIBLE 7

    // Keep selected slot visible
    if (browser_slot < browser_scroll) {
        browser_scroll = browser_slot;
    }
    if (browser_slot >= browser_scroll + BROWSER_VISIBLE) {
        browser_scroll = browser_slot - BROWSER_VISIBLE + 1;
    }

    // Header line
    region_fill(&line[0], 0);
    if (browser_status_timer > 0) {
        browser_status_timer--;
        font_string_region_clip(&line[0], browser_status, 0, 0, 0xf, 0);
    } else if (browser_editing) {
        font_string_region_clip(&line[0], "RENAME SCENE", 0, 0, 0xf, 0);
    } else {
        font_string_region_clip(&line[0], "SCENES ent/s/r/c/del", 0, 0, 0x8, 0);
    }

    for (uint8_t i = 0; i < BROWSER_VISIBLE; i++) {
        uint8_t slot = browser_scroll + i;
        region_fill(&line[i + 1], 0);

        if (slot >= SCENE_SLOTS) continue;

        uint8_t is_used = !flash_is_scene_empty(slot);

        // Format: "01 Scene Name"
        char buf[VIEW_W + 1];
        memset(buf, ' ', VIEW_W);
        buf[VIEW_W] = '\0';

        // Slot number
        buf[0] = '0' + (slot / 10);
        buf[1] = '0' + (slot % 10);
        buf[2] = ' ';

        if (browser_editing && slot == browser_slot) {
            // Show edit buffer with cursor
            for (uint8_t j = 0; j < SCENE_NAME_LEN && browser_name[j]; j++) {
                buf[3 + j] = browser_name[j];
            }
        } else {
            // Show stored name, or "(empty)" / "(used)"
            const char *name = f.scenes[slot].name;
            if (name[0] != '\0') {
                for (uint8_t j = 0; j < SCENE_NAME_LEN && name[j]; j++) {
                    buf[3 + j] = name[j];
                }
            } else if (is_used) {
                const char *tag = "(unnamed)";
                for (uint8_t j = 0; tag[j]; j++) buf[3 + j] = tag[j];
            }
        }

        uint8_t is_selected = (slot == browser_slot);
        uint8_t base_fg = is_selected ? 0xf : (is_used ? 0xa : 0x6);
        uint8_t base_bg = is_selected ? 0x3 : 0;

        // Draw character by character for precise cursor alignment
        for (uint8_t j = 0; j < VIEW_W; j++) {
            char ch[2] = { buf[j], '\0' };
            uint8_t fg = base_fg;
            uint8_t bg = base_bg;

            // Invert at cursor position when editing
            if (browser_editing && is_selected && j == 3 + browser_name_cursor) {
                fg = 0;
                bg = 0xf;
            }

            font_string_region_clip(&line[i + 1], ch, j * CHAR_W, 0, fg, bg);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Scene browser key handling

static void browser_start_rename(void) {
    browser_editing = 1;
    // Copy current name into edit buffer
    const char *name = f.scenes[browser_slot].name;
    memset(browser_name, 0, sizeof(browser_name));
    for (uint8_t i = 0; i < SCENE_NAME_LEN && name[i]; i++) {
        browser_name[i] = name[i];
    }
    browser_name_cursor = 0;
    // Move cursor to end of existing name
    while (browser_name_cursor < SCENE_NAME_LEN && browser_name[browser_name_cursor]) {
        browser_name_cursor++;
    }
}

static void browser_finish_rename(void) {
    flash_save_scene_name(browser_slot, browser_name);
    browser_editing = 0;
    browser_status = "SCENE RENAMED";
    browser_status_timer = BROWSER_STATUS_FRAMES;
}

static void browser_cancel_rename(void) {
    browser_editing = 0;
}

static void process_browser_key(uint8_t key, uint8_t mod) {
    if (browser_editing) {
        // Name editing mode
        if (key == HID_ESC) {
            browser_cancel_rename();
            return;
        }
        if (key == HID_ENTER) {
            browser_finish_rename();
            return;
        }
        if (key == HID_BKSP) {
            if (browser_name_cursor > 0) {
                browser_name_cursor--;
                // Shift remaining chars left
                for (uint8_t i = browser_name_cursor; i < SCENE_NAME_LEN; i++) {
                    browser_name[i] = browser_name[i + 1];
                }
            }
            return;
        }
        if (key == HID_LEFT) {
            if (browser_name_cursor > 0) browser_name_cursor--;
            return;
        }
        if (key == HID_RIGHT) {
            if (browser_name_cursor < SCENE_NAME_LEN - 1 && browser_name[browser_name_cursor]) {
                browser_name_cursor++;
            }
            return;
        }

        // Typeable characters
        char c = 0;
        if (key >= 0x04 && key <= 0x1D) {
            c = 'a' + (key - 0x04);
            if (mod & MOD_SHIFT) c = c - 'a' + 'A';
        } else if (key >= 0x1E && key <= 0x27) {
            c = (key == 0x27) ? '0' : ('1' + (key - 0x1E));
        } else if (key == HID_SPACE) {
            c = ' ';
        } else if (key == 0x2D) { // minus
            c = (mod & MOD_SHIFT) ? '_' : '-';
        } else if (key == 0x37) { // period
            c = '.';
        }

        if (c && browser_name_cursor < SCENE_NAME_LEN) {
            // Insert character (shift right)
            for (uint8_t i = SCENE_NAME_LEN - 1; i > browser_name_cursor; i--) {
                browser_name[i] = browser_name[i - 1];
            }
            browser_name[browser_name_cursor] = c;
            browser_name[SCENE_NAME_LEN] = '\0';
            if (browser_name_cursor < SCENE_NAME_LEN - 1) {
                browser_name_cursor++;
            }
        }
        return;
    }

    // Browser navigation mode
    if (key == HID_ESC) {
        ui_mode = MODE_GRID;
        return;
    }
    if (key == HID_UP) {
        if (browser_slot > 0) browser_slot--;
        return;
    }
    if (key == HID_DOWN) {
        if (browser_slot < SCENE_SLOTS - 1) browser_slot++;
        return;
    }
    // Enter: load scene
    if (key == HID_ENTER) {
        set_playing(0);  // first, so playback's undo step is the old scene's
        flash_load_scene(browser_slot);
        flash_save_last_scene(browser_slot);
        metro_restart();
        cursor_x = 0;
        cursor_y = 0;
        view_x = 0;
        view_y = 0;
        ui_mode = MODE_GRID;
        return;
    }
    // S: save current grid to selected slot
    if (key == 0x16 && !(mod & MOD_SHIFT)) { // 's'
        flash_save_scene(browser_slot);
        flash_save_last_scene(browser_slot);
        // Auto-enter rename if scene has no name yet
        if (f.scenes[browser_slot].name[0] == '\0') {
            browser_start_rename();
        } else {
            browser_status = "SCENE SAVED";
            browser_status_timer = BROWSER_STATUS_FRAMES;
        }
        return;
    }
    // R or F2: rename
    if (key == 0x15 || key == 0x3B) { // 'r' or F2
        browser_start_rename();
        return;
    }
    // C: CV calibration
    if (key == 0x06) {
        cal_enter();
        return;
    }
    // Delete/Ctrl+Backspace: clear scene
    if (key == 0x4C || (key == HID_BKSP && (mod & MOD_CTRL))) { // delete key
        flash_clear_scene(browser_slot);
        browser_status = "SCENE CLEARED";
        browser_status_timer = BROWSER_STATUS_FRAMES;
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////
// CV calibration screen
//
// The selected channel outputs the raw code of the selected point (1 V or
// 4 V). Adjust it until a meter reads 1.000 / 4.000 V (or type the measured
// mV and press Enter), or by ear: tune the VCO to C at 1 V, then nudge 4 V
// until it plays C three octaves up. Esc saves to flash.

#define CAL_MV_DIGITS 4

static uint8_t cal_ch = 0;
static uint8_t cal_point = 0;  // 0 = 1 V, 1 = 4 V
static char cal_mv[CAL_MV_DIGITS + 1];
static uint8_t cal_mv_len = 0;

static uint16_t *cal_point_code(void) {
    return cal_point ? &cal.code_4v[cal_ch] : &cal.code_1v[cal_ch];
}

static void cal_output(void) {
    dac_values[cal_ch] = *cal_point_code();
    dac_update();
}

static void cal_nudge(int16_t delta) {
    uint16_t *code = cal_point_code();
    int32_t v = (int32_t)*code + delta;
    if (v < 0) v = 0;
    if (v > DAC_MAX) v = DAC_MAX;
    *code = (uint16_t)v;
    cal_output();
}

static void cal_enter(void) {
    set_playing(0);
    cal_ch = 0;
    cal_point = 0;
    cal_mv_len = 0;
    cal_mv[0] = '\0';
    ui_mode = MODE_CAL;
    cal_output();
}

// Measured mV at the current point -> move the code by the difference,
// assuming the nominal 5000 mV / 4096 codes slope. Repeat to refine.
static void cal_apply_measurement(void) {
    if (cal_mv_len == 0) return;
    int32_t measured = 0;
    for (uint8_t i = 0; i < cal_mv_len; i++) measured = measured * 10 + (cal_mv[i] - '0');
    int32_t target = cal_point ? 4000 : 1000;
    int32_t num = (target - measured) * (DAC_MAX + 1);
    int32_t delta = num >= 0 ? (num + 2500) / 5000 : -((-num + 2500) / 5000);
    cal_mv_len = 0;
    cal_mv[0] = '\0';
    cal_nudge((int16_t)delta);
}

static void process_cal_key(uint8_t key, uint8_t mod) {
    int16_t step = (mod & MOD_SHIFT) ? 10 : 1;
    if (key == HID_UP)   { cal_nudge(step); return; }
    if (key == HID_DOWN) { cal_nudge(-step); return; }
    if (key == HID_LEFT || key == HID_RIGHT) {
        cal_ch = (cal_ch + (key == HID_RIGHT ? 1 : 3)) % 4;
        cal_output();
        return;
    }
    if (key == HID_TAB) {
        cal_point = !cal_point;
        cal_output();
        return;
    }
    if (key >= 0x1E && key <= 0x27) {  // digits
        if (cal_mv_len < CAL_MV_DIGITS) {
            cal_mv[cal_mv_len++] = (key == 0x27) ? '0' : ('1' + (key - 0x1E));
            cal_mv[cal_mv_len] = '\0';
        }
        return;
    }
    if (key == HID_BKSP) {
        if (cal_mv_len > 0) cal_mv[--cal_mv_len] = '\0';
        return;
    }
    if (key == HID_ENTER) { cal_apply_measurement(); return; }
    if (key == 0x15) {  // 'r': reset this channel
        cal_reset_channel(cal_ch);
        cal_output();
        return;
    }
    if (key == HID_ESC) {
        cal_save();
        ui_mode = MODE_GRID;
        set_status("CALIBRATION SAVED");
        return;
    }
}

static void put_num4(char *buf, uint16_t v) {
    buf[0] = '0' + (v / 1000) % 10;
    buf[1] = '0' + (v / 100) % 10;
    buf[2] = '0' + (v / 10) % 10;
    buf[3] = '0' + v % 10;
}

static void render_cal(void) {
    char buf[VIEW_W + 1];

    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    strcpy(buf, "CALIBRATE CV 1");
    buf[13] = '1' + cal_ch;
    font_string_region_clip(&line[0], buf, 0, 0, 0xf, 0);

    for (uint8_t p = 0; p < 2; p++) {
        strcpy(buf, "  1V  code 0000");
        if (p == cal_point) buf[0] = '>';
        if (p) buf[2] = '4';
        put_num4(&buf[11], p ? cal.code_4v[cal_ch] : cal.code_1v[cal_ch]);
        font_string_region_clip(&line[1 + p], buf, 0, 0,
                                p == cal_point ? 0xf : 0x6, 0);
    }

    strcpy(buf, "measured: ");
    strcat(buf, cal_mv);
    strcat(buf, "_ mV");
    font_string_region_clip(&line[3], buf, 0, 0, 0xc, 0);

    font_string_region_clip(&line[4], "up/dn nudge sh x10", 0, 0, 0x6, 0);
    font_string_region_clip(&line[5], "tab 1V/4V  </> ch", 0, 0, 0x6, 0);
    font_string_region_clip(&line[6], "enter apply  r reset", 0, 0, 0x6, 0);
    font_string_region_clip(&line[7], "esc save + exit", 0, 0, 0x6, 0);
}

////////////////////////////////////////////////////////////////////////////////
// Gate/trigger management
//
// Each TR output has a gate counter, counting the ticks the gate stays open.
// A gate closes at the end of the tick in which its counter reaches 0, so a
// note on that same tick keeps it open.
//
// Retrigger: a note that fires while its gate is open drops the gate for
// 2-3 ms so envelopes see a new rising edge (like orca-c's note-off before
// note-on). gateTimer, running every 1 ms in interrupt context, raises it
// again.

#define GATE_RETRIG_MS 3  // counted by the 1 ms timer: gap of 2-3 ms

static uint8_t gate_counter[4] = {0, 0, 0, 0};
static uint8_t gate_open[4] = {0, 0, 0, 0};
static volatile uint8_t gate_retrig[4] = {0, 0, 0, 0};  // ms until raised

static const uint32_t tr_pins[4] = { B08, B09, B10, B11 };

static void gate_set(uint8_t ch, uint8_t duration) {
    if (ch >= 4) return;
    if (gate_open[ch]) {
        // set the countdown before dropping the pin so the timer can't
        // raise it in between
        gate_retrig[ch] = GATE_RETRIG_MS;
        gpio_clr_gpio_pin(tr_pins[ch]);
    } else {
        gate_open[ch] = 1;
        gpio_set_gpio_pin(tr_pins[ch]);
    }
    gate_counter[ch] = duration;
}

static void gate_close(uint8_t ch) {
    gate_counter[ch] = 0;
    gate_open[ch] = 0;
    gate_retrig[ch] = 0;
    gpio_clr_gpio_pin(tr_pins[ch]);
}

// Called at the start of each tick to advance gate counters
static void gates_tick(void) {
    for (uint8_t i = 0; i < 4; i++) {
        if (gate_counter[i] > 0) gate_counter[i]--;
    }
}

// Called after a tick's events: close gates that ran out
static void gates_close_expired(void) {
    for (uint8_t i = 0; i < 4; i++) {
        if (gate_open[i] && gate_counter[i] == 0) gate_close(i);
    }
}

static void gates_all_off(void) {
    for (uint8_t i = 0; i < 4; i++) gate_close(i);
}

static void set_playing(uint8_t on) {
    if (on && !playing) undo_play_started();
    if (!on && playing) undo_play_stopped();
    playing = on;
    if (!on) gates_all_off();
}

////////////////////////////////////////////////////////////////////////////////
// Process ORCA output events -> CV/trigger

static void process_orca_events(void) {
    for (Usz i = 0; i < oevent_list.count; i++) {
        Oevent *ev = &oevent_list.buffer[i];
        switch (ev->any.oevent_type) {
        case Oevent_type_midi_note: {
            // Map MIDI note events to CV + trigger/gate
            // Channel 0-3 -> CV/TR output 1-4
            Oevent_midi_note *n = &ev->midi_note;
            if (n->channel < 4) {
                // 1 V/oct, note number = octave*12 + note
                uint16_t note_num = (uint16_t)n->octave * 12 + n->note;
                dac_set(n->channel, note_to_dac(quantize(note_num)));

                // Duration: 0 = 1-tick trigger pulse, 1-z = gate held for N ticks
                uint8_t dur = n->duration;
                if (dur == 0) dur = 1;
                gate_set(n->channel, dur);
            }
            break;
        }
        case Oevent_type_midi_cc: {
            // Map CC to CV output (no trigger)
            Oevent_midi_cc *cc = &ev->midi_cc;
            if (cc->channel < 4) {
                // 0-127 -> 0-4095 (0-5 V)
                uint16_t cv = ((uint32_t)cc->value * DAC_MAX + 63) / 127;
                dac_set(cc->channel, cv);
            }
            break;
        }
        case Oevent_type_midi_pb: {
            // Fine CV (no trigger): msb*36 + lsb, 1296 steps over 0-5 V.
            // The VM scales each glyph 0-35 to 0-127 (index*127/35); undo
            // that to get the glyph values back.
            Oevent_midi_pb *pb = &ev->midi_pb;
            if (pb->channel < 4) {
                uint32_t msb = ((uint32_t)pb->msb * 35 + 126) / 127;
                uint32_t lsb = ((uint32_t)pb->lsb * 35 + 126) / 127;
                uint32_t steps = msb * 36 + lsb;  // 0-1295
                dac_set(pb->channel, (steps * DAC_MAX + 647) / 1295);
            }
            break;
        }
        default:
            break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Run one ORCA tick

static void orca_tick(void) {
    read_inputs();  // fresh CV IN for the & operator
    gates_tick();
    mbuffer_clear(marks, GRID_H, GRID_W);
    oevent_list_clear(&oevent_list);
    orca_run(grid, marks, GRID_H, GRID_W, tick_number, &oevent_list,
             random_seed);
    process_orca_events();
    gates_close_expired();
    tick_number++;
    needs_remark = 1;
}

// Highlights: like orca-c, run the VM on a scratch copy of the grid so the
// marks (ports, locks) match what the next tick will do. The grid isn't
// changed and the events are discarded.
static Glyph remark_grid[GRID_H * GRID_W];
static Oevent_list remark_events;

static void remark(void) {
    memcpy(remark_grid, grid, sizeof(grid));
    mbuffer_clear(marks, GRID_H, GRID_W);
    oevent_list_clear(&remark_events);
    orca_run(remark_grid, marks, GRID_H, GRID_W, tick_number, &remark_events,
             random_seed);
}

////////////////////////////////////////////////////////////////////////////////
// Timer callbacks

static void refreshTimer_callback(void *o) {
    (void)o;
    event_t e = { .type = kEventScreenRefresh, .data = 0 };
    event_post(&e);
}

static void keyTimer_callback(void *o) {
    (void)o;
    event_t e = { .type = kEventKeyTimer, .data = 0 };
    event_post(&e);
}

static void metroTimer_callback(void *o) {
    (void)o;
    event_t e = { .type = kEventTimer, .data = 0 };
    event_post(&e);
}

static void hidTimer_callback(void *o) {
    (void)o;
    event_t e = { .type = kEventHidTimer, .data = 0 };
    event_post(&e);
}

// Interrupt context, every 1 ms: raise retriggered gates after their gap
static void gateTimer_callback(void *o) {
    (void)o;
    for (uint8_t i = 0; i < 4; i++) {
        if (gate_retrig[i] && --gate_retrig[i] == 0) {
            gpio_set_gpio_pin(tr_pins[i]);
        }
    }
}

static void metro_restart(void) {
    timer_remove(&metroTimer);
    timer_add(&metroTimer, metro_ms, &metroTimer_callback, NULL);
}

////////////////////////////////////////////////////////////////////////////////
// Keyboard input handling

static int8_t old_frame[8] = {0};
static int8_t hold_key = 0;
static uint8_t hold_mod = 0;
static uint16_t hold_count = 0;

#define KEY_REPEAT_DELAY 12  // ~560ms before repeat starts (at 47ms timer)
#define KEY_REPEAT_RATE  2   // ~94ms between repeats

// Fill the selection (or just the cursor cell) with one glyph, as one undo
// step. Like orca-c's default (overwrite) mode, the cursor stays put.
static void fill_selection(Glyph g) {
    undo_begin();
    for (uint8_t y = cursor_y; y < cursor_y + sel_h && y < GRID_H; y++) {
        for (uint8_t x = cursor_x; x < cursor_x + sel_w && x < GRID_W; x++) {
            grid_write(y * GRID_W + x, g);
        }
    }
    undo_end();
}

static void place_glyph(char c) {
    if (append_mode) {
        undo_begin();
        grid_write(cursor_y * GRID_W + cursor_x, c);
        undo_end();
        if (cursor_x < GRID_W - 1) cursor_x++;
    } else {
        fill_selection(c);
    }
}

// Clipboard for Ctrl+C / Ctrl+X / Ctrl+V
static Glyph clip[GRID_H * GRID_W];
static uint8_t clip_w = 0;
static uint8_t clip_h = 0;

static void copy_selection(void) {
    clip_w = sel_w;
    clip_h = sel_h;
    if (cursor_x + clip_w > GRID_W) clip_w = GRID_W - cursor_x;
    if (cursor_y + clip_h > GRID_H) clip_h = GRID_H - cursor_y;
    for (uint8_t y = 0; y < clip_h; y++) {
        memcpy(&clip[y * clip_w], &grid[(cursor_y + y) * GRID_W + cursor_x], clip_w);
    }
}

// Paste at the cursor (clipped to the grid); the pasted area is selected
static void paste_clipboard(void) {
    if (clip_w == 0) return;
    uint8_t w = clip_w, h = clip_h;
    if (cursor_x + w > GRID_W) w = GRID_W - cursor_x;
    if (cursor_y + h > GRID_H) h = GRID_H - cursor_y;
    undo_begin();
    for (uint8_t y = 0; y < h; y++) {
        for (uint8_t x = 0; x < w; x++) {
            grid_write((cursor_y + y) * GRID_W + cursor_x + x, clip[y * clip_w + x]);
        }
    }
    undo_end();
    sel_w = w;
    sel_h = h;
}

static void change_tempo(int8_t delta) {
    int16_t ms = (int16_t)metro_ms + delta;
    if (ms < 20) ms = 20;
    if (ms > 250) ms = 250;
    metro_ms = (uint8_t)ms;
    metro_restart();
    // "100ms 4/4=150BPM"
    static char tempo_buf[20] = "000ms 4/4=000BPM";
    put_digits(&tempo_buf[0], metro_ms, 3);
    put_digits(&tempo_buf[10], 15000 / metro_ms, 3);  // 4 ticks per beat
    set_status(tempo_buf);
}

static void process_key(uint8_t key, uint8_t mod) {
    // < / >: slower / faster tempo (Shift+comma / Shift+period)
    if ((key == 0x36 || key == 0x37) && (mod & MOD_SHIFT)) {
        change_tempo(key == 0x37 ? -1 : 1);
        return;
    }

    // Shift+arrows: grow/shrink the selection right and down from the cursor
    if (mod & MOD_SHIFT) {
        if (key == HID_RIGHT) { if (cursor_x + sel_w < GRID_W) sel_w++; return; }
        if (key == HID_LEFT)  { if (sel_w > 1) sel_w--; return; }
        if (key == HID_DOWN)  { if (cursor_y + sel_h < GRID_H) sel_h++; return; }
        if (key == HID_UP)    { if (sel_h > 1) sel_h--; return; }
    }

    // Arrow keys: move cursor (a selection moves with it)
    if (key == HID_UP)    { if (cursor_y > 0) cursor_y--; return; }
    if (key == HID_DOWN)  { if (cursor_y < GRID_H - 1) cursor_y++; return; }
    if (key == HID_LEFT)  { if (cursor_x > 0) cursor_x--; return; }
    if (key == HID_RIGHT) { if (cursor_x < GRID_W - 1) cursor_x++; return; }

    // Insert / Ctrl+I: toggle append mode
    if (key == 0x49 || ((mod & MOD_CTRL) && key == 0x0C)) {
        append_mode = !append_mode;
        set_status(append_mode ? "APPEND MODE" : "OVERWRITE MODE");
        return;
    }

    // Ctrl shortcuts; other Ctrl+letters do nothing
    if ((mod & MOD_CTRL) && key >= 0x04 && key <= 0x1D) {
        switch (key) {
        case 0x1D: undo_pop(); break;                           // Ctrl+Z
        case 0x06: copy_selection(); set_status("COPIED"); break;  // Ctrl+C
        case 0x1B:                                              // Ctrl+X
            copy_selection();
            fill_selection('.');
            set_status("CUT");
            break;
        case 0x19: paste_clipboard(); break;                    // Ctrl+V
        default: break;
        }
        return;
    }

    // F1: toggle the status line
    if (key == 0x3A) { hud_on = !hud_on; return; }

    // Space: toggle play/pause
    // (in append mode Space types a dot instead, like orca-c)
    if (key == HID_SPACE) {
        if (append_mode) place_glyph('.');
        else set_playing(!playing);
        return;
    }

    // Tab: manual single tick (only while paused, so it can't add ticks).
    // Its changes to the grid are one undo step.
    if (key == HID_TAB) {
        if (!playing) {
            undo_play_started();
            orca_tick();
            undo_play_stopped();
        }
        return;
    }

    // Backspace: clear cell and move left; with a selection, clear it.
    // In append mode: move left, then clear that cell.
    if (key == HID_BKSP) {
        if (append_mode) {
            if (cursor_x > 0) cursor_x--;
            undo_begin();
            grid_write(cursor_y * GRID_W + cursor_x, '.');
            undo_end();
            return;
        }
        fill_selection('.');
        if (sel_w == 1 && sel_h == 1 && cursor_x > 0) cursor_x--;
        return;
    }

    // Delete: clear the cell or selection
    if (key == 0x4C) { fill_selection('.'); return; }

    // Escape: drop the selection, or open the scene browser
    if (key == HID_ESC) {
        if (mod & MOD_CTRL) {
            // Ctrl+Escape: clear the grid (one undo step)
            undo_begin();
            for (uint16_t i = 0; i < GRID_W * GRID_H; i++) grid_write(i, '.');
            undo_end();
            tick_number = 0;
            gates_all_off();
            return;
        }
        if (sel_w > 1 || sel_h > 1) {
            sel_w = sel_h = 1;
            return;
        }
        ui_mode = MODE_BROWSER;
        browser_editing = 0;
        return;
    }

    // Shifted number keys -> ORCA special characters
    if (mod & MOD_SHIFT) {
        switch (key) {
        case 0x1E: place_glyph('!'); return; // shift+1
        case 0x20: place_glyph('#'); return; // shift+3
        case 0x22: place_glyph('%'); return; // shift+5
        case 0x24: place_glyph('&'); return; // shift+7
        case 0x25: place_glyph('*'); return; // shift+8
        default: break;
        }
    }

    // Letter keys (a-z): HID codes 0x04-0x1D
    if (key >= 0x04 && key <= 0x1D) {
        char c = 'a' + (key - 0x04);
        if (mod & MOD_SHIFT) c = c - 'a' + 'A';
        place_glyph(c);
        return;
    }

    // Number keys (0-9): HID codes 0x1E-0x27
    if (key >= 0x1E && key <= 0x27) {
        char c = (key == 0x27) ? '0' : ('1' + (key - 0x1E));
        place_glyph(c);
        return;
    }

    // Period
    if (key == 0x37) { place_glyph('.'); return; }

    // Semicolon / colon
    if (key == 0x33) {
        place_glyph((mod & MOD_SHIFT) ? ':' : ';');
        return;
    }

    // Equals
    if (key == 0x2E) { place_glyph('='); return; }

    // Question mark (shift+/)
    if (key == 0x38 && (mod & MOD_SHIFT)) { place_glyph('?'); return; }
}

// Check if key was present in old frame
static uint8_t frame_compare(int8_t key) {
    for (uint8_t i = 2; i < 8; i++) {
        if (old_frame[i] == key) return 1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Event handlers

static void handler_None(s32 data) {
    (void)data;
}

// Front button: held, the knob sets swing (data = pin level, 0 = pressed)
static void handler_Front(s32 data) {
    uint8_t held = (data == 0);
    if (held == front_held) return;
    front_held = held;
    knob_mode_changed();
    if (held) show_swing();
}

static void handler_ScreenRefresh(s32 data) {
    (void)data;
    if (needs_remark) {
        remark();
        needs_remark = 0;
    }
    if (ui_mode == MODE_BROWSER) {
        render_browser();
    } else if (ui_mode == MODE_CAL) {
        render_cal();
    } else {
        render_grid();
    }
    draw_all();
}

static void handler_PollADC(s32 data) {
    (void)data;
    read_inputs();
}

static void handler_KeyTimer(s32 data) {
    (void)data;
}

static void handler_HidConnect(s32 data) {
    (void)data;
    print_dbg("\r\nhid connect");
    timer_add(&hidTimer, 47, &hidTimer_callback, NULL);
}

static void handler_HidDisconnect(s32 data) {
    (void)data;
    print_dbg("\r\nhid disconnect");
    timer_remove(&hidTimer);
}

static void dispatch_key(uint8_t key, uint8_t mod) {
    if (ui_mode == MODE_BROWSER)
        process_browser_key(key, mod);
    else if (ui_mode == MODE_CAL)
        process_cal_key(key, mod);
    else
        process_key(key, mod);
}

static uint8_t is_repeatable(uint8_t key, uint8_t mod) {
    return key == HID_UP || key == HID_DOWN || key == HID_LEFT || key == HID_RIGHT
        || key == HID_BKSP
        || ((key == 0x36 || key == 0x37) && (mod & MOD_SHIFT));  // < and >
}

static void handler_HidTimer(s32 data) {
    (void)data;
    const int8_t *frame = (const int8_t *)hid_get_frame_data();

    if (hid_get_frame_dirty()) {
        uint8_t mod = frame[0];

        for (uint8_t i = 2; i < 8; i++) {
            if (frame[i] == 0) {
                if (i == 2) {
                    // All keys released
                    hold_key = 0;
                    hold_count = 0;
                }
                break;
            }
            if (!frame_compare(frame[i])) {
                dispatch_key(frame[i], mod);
                hold_key = frame[i];
                hold_count = 0;
            }
        }
        hold_mod = mod;

        // If the repeating key was released while others are still held,
        // repeat the newest key that is still down instead
        if (hold_key) {
            int8_t still_down = 0;
            for (uint8_t i = 2; i < 8 && frame[i]; i++) {
                if (frame[i] == hold_key) { still_down = hold_key; break; }
                still_down = frame[i];
            }
            if (still_down != hold_key) {
                hold_key = still_down;
                hold_count = 0;
            }
        }

        for (uint8_t i = 0; i < 8; i++) {
            old_frame[i] = frame[i];
        }
        hid_clear_frame_dirty();
    } else if (hold_key && is_repeatable(hold_key, hold_mod)) {
        // Key repeat for held keys
        hold_count++;
        if (hold_count >= KEY_REPEAT_DELAY &&
            (hold_count - KEY_REPEAT_DELAY) % KEY_REPEAT_RATE == 0) {
            dispatch_key(hold_key, hold_mod);
        }
    }
}

// Trigger inputs: In 1 = clock, In 2 = reset. While clock edges keep
// arriving the internal metro is muted; it takes over again after
// EXT_CLOCK_TIMEOUT_MS without an edge.
#define CLOCK_INPUT 0
#define RESET_INPUT 1
#define EXT_CLOCK_TIMEOUT_MS 2000

static uint8_t ext_clock_seen = 0;
static u32 ext_clock_last = 0;

static uint8_t ext_clock_active(void) {
    return ext_clock_seen && time_now() - ext_clock_last < EXT_CLOCK_TIMEOUT_MS;
}

static void handler_Trigger(s32 data) {
    // Inputs interrupt on both edges: act on rising edges only
    if (!gpio_get_pin_value(A00 + data)) return;

    if (data == CLOCK_INPUT) {
        ext_clock_seen = 1;
        ext_clock_last = time_now();
        if (playing) orca_tick();
    } else if (data == RESET_INPUT) {
        tick_number = 0;
    }
}

// Swing: after each internal tick, set the gap to the next one. Adjust the
// timer's remaining count by the change in length (rather than restarting it)
// so the time it took to get here doesn't add up.
static void metro_swing_next(void) {
    uint16_t pair = 2 * metro_ms;
    uint16_t first = (pair * swing_pct + 50) / 100;  // even tick -> odd tick
    u32 len = (tick_number & 1) ? first : pair - first;
    u8 irq = irqs_pause();
    int32_t remain = (int32_t)metroTimer.ticksRemain + (int32_t)len
                     - (int32_t)metroTimer.ticks;
    metroTimer.ticks = len;
    metroTimer.ticksRemain = remain < 1 ? 1 : remain;
    irqs_resume(irq);
}

static void handler_Timer(s32 data) {
    (void)data;
    if (playing && !ext_clock_active()) {
        orca_tick();
        metro_swing_next();
    }
}

////////////////////////////////////////////////////////////////////////////////
// Event setup

static void assign_event_handlers(void) {
    for (size_t i = 0; i < kNumEventTypes; i++) {
        app_event_handlers[i] = &handler_None;
    }

    app_event_handlers[kEventFront] = &handler_Front;
    app_event_handlers[kEventKeyTimer] = &handler_KeyTimer;
    app_event_handlers[kEventHidConnect] = &handler_HidConnect;
    app_event_handlers[kEventHidDisconnect] = &handler_HidDisconnect;
    app_event_handlers[kEventHidTimer] = &handler_HidTimer;
    app_event_handlers[kEventScreenRefresh] = &handler_ScreenRefresh;
    app_event_handlers[kEventTrigger] = &handler_Trigger;
    app_event_handlers[kEventTimer] = &handler_Timer;
    app_event_handlers[kEventPollADC] = &handler_PollADC;
}

////////////////////////////////////////////////////////////////////////////////
// main

int main(void) {
    sysclk_init();
    init_dbg_rs232(FMCK_HZ);
    init_gpio();
    assign_event_handlers();
    init_events();
    init_tc();
    init_spi();
    init_adc();

    irq_initialize_vectors();
    register_interrupts();
    cpu_irq_enable();

    init_usb_host();
    init_oled();

    delay_ms(500);
    init_i2c_leader();

    print_dbg("\r\n\r\n// orca firmware\r\n");

    // Setup DAC daisy chain
    spi_selectChip(DAC_SPI, DAC_SPI_NPCS);
    spi_write(DAC_SPI, 0x80);
    spi_write(DAC_SPI, 0xff);
    spi_write(DAC_SPI, 0xff);
    spi_unselectChip(DAC_SPI, DAC_SPI_NPCS);

    // Init screen regions
    for (int i = 0; i < 8; i++) {
        region_alloc(&line[i]);
    }

    // Init flash and load last scene
    flash_prepare();
    cal_load();
    grid_init();

    // Load last saved scene (if any)
    uint8_t last = f.last_scene;
    if (last < SCENE_SLOTS && !flash_is_scene_empty(last)) {
        flash_load_scene(last);
        browser_slot = last;
    }
    undo_clear();  // nothing to undo at power-on

    // Initial render
    render_grid();
    draw_all();

    // Start timers
    timer_add(&refreshTimer, 63, &refreshTimer_callback, NULL);  // ~15 Hz screen
    timer_add(&keyTimer, 71, &keyTimer_callback, NULL);
    timer_add(&gateTimer, 1, &gateTimer_callback, NULL);
    timer_add(&adcTimer, ADC_POLL_MS, &adcTimer_callback, NULL);
    timer_add(&metroTimer, metro_ms, &metroTimer_callback, NULL);

    print_dbg("\r\norca ready\r\n");

    // Event loop
    event_t e;
    while (1) {
        if (event_next(&e)) {
            (app_event_handlers)[e.type](e.data);
        }
    }
}
