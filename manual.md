# ORCA for Teletype — Operator Manual

ORCA is a two-dimensional esoteric programming language designed for live coding music. This is a port of [Orca-c](https://github.com/hundredrabbits/Orca-c) to the monome Teletype, set up for the AE Modular Telex (0–5 V).

The grid is 64x64 characters. The OLED displays a 25x8 viewport (25x7 with the status line) that scrolls to follow the cursor.

## Keyboard Controls

| Key | Action |
|---|---|
| Arrow keys | Move cursor (hold to repeat); a selection moves with it |
| Shift+Arrow keys | Grow / shrink the selection right and down from the cursor |
| Letters / numbers | Place operator or value in the cell, or fill the whole selection (the cursor stays in place) |
| Backspace | Clear cell and move left (hold to repeat); with a selection, clear it |
| Delete | Clear the cell or selection |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste the selection (paste at the cursor; the pasted area becomes the selection) |
| Ctrl+Z | Undo (64 steps) |
| Space | Toggle play / pause (pausing closes all gates); in append mode, types `.` |
| Insert / Ctrl+I | Toggle append mode (see below) |
| Tab | Advance one tick manually (while paused) |
| `<` / `>` | Slower / faster tempo, 1 ms per tick per press (20–250 ms per tick; hold to repeat) |
| F1 | Show / hide the status line |
| Escape | Drop the selection; with no selection, open the scene browser (then **c** for CV calibration) |
| Front button (hold) | Turn PARAM to set swing (see [Swing](#swing)) |
| Ctrl+Escape | Clear entire grid (closes all gates; undoable) |

**Append mode** (Insert or Ctrl+I, like orca-c): typing writes the cell and moves the cursor right, Backspace moves
left and clears that cell, and Space types `.` (so use Space to play/pause only in the normal, overwrite mode). The
status line shows `A` after the play state while it's on, and the mode change is announced on the bottom row.

**Undo** covers typing, clearing, cut, paste, loading a scene and clearing the grid. It also covers what playback
did: starting play and then stopping is one step, so Ctrl+Z after pausing puts the grid back as it was before you
pressed play. Tab steps are undoable too.

## Status Line

**F1** toggles a status line in the bottom row (the grid then shows 7 rows):

```
> 0123 150b 12,05 MAJ
│ │    │    │     └── scale (PARAM knob)
│ │    │    └──────── cursor x,y
│ │    └───────────── tempo in BPM (4 ticks per beat)
│ └────────────────── tick counter
└──────────────────── > playing, E playing on external clock, | paused (followed by A in append mode)
```

Messages (tempo, scale, copy/cut, undo, calibration saved) also appear on this row for about 2 seconds. With the
status line hidden, they briefly cover the bottom grid row.

Shift+number enters ORCA special characters: `!` `#` `%` `&` `*`. Shift+`;` gives `:`, Shift+`/` gives `?`.

| Knob | Action |
|---|---|
| PARAM | Selects the scale that notes snap to (see [Scale Quantizer](#scale-quantizer)) |
| Front button + PARAM | Sets swing 50–75 % |

After you press or release the front button, PARAM only takes effect once you turn it, so the scale (or swing)
doesn't jump to wherever the knob was left.

## How ORCA Works

The grid is evaluated once per **tick**, top-to-bottom, left-to-right. Each cell contains either a dot `.` (empty) or an operator character. Operators read from **input** cells and write to **output** cells relative to their position. A tick advances the clock, updates all operators, and produces output events.

Uppercase operators are always active. Lowercase operators require a neighboring **bang** (`*`) to activate.

Values in ORCA are base-36: `0`–`9` then `a`–`z` (10–35).

## Teletype Output Operators

These operators produce CV and trigger output from the Teletype's jacks.

### `:` — CV Note + Trigger

Sends a 1V/octave CV and fires a trigger pulse. Requires a neighboring `*` bang to activate.

```
 :03Cgd
 │││││
 ││││└── duration: gate length in ticks
 │││└─── velocity: 1-z (0 = silent, . = full)
 ││└──── note: C D E F G A B (lowercase = sharp)
 │└───── octave: 0-9
 └────── channel: 0-3 → CV/TR output 1-4
```

**Channel mapping:** `0` = CV 1 + TR 1, `1` = CV 2 + TR 2, `2` = CV 3 + TR 3, `3` = CV 4 + TR 4

**CV scaling:** 1V/octave over the AE Modular 0–5 V range. Octave 0 note C = 0 V, octave 1 C = 1 V, … octave 5 C =
5 V. Anything higher (octave 5 above C, octaves 6–9) is clamped to 5 V. Keep pitches a little above 0 V and below
~4.75 V for best accuracy. Notes are snapped to the scale chosen with the PARAM knob first.

**Duration / gate length:**
- `.` or `0` — short trigger pulse (HIGH for 1 tick)
- `1`–`z` (1–35) — gate held HIGH for that many ticks

If a new note fires on the same channel while a gate is still open, the gate drops low for ~2 ms and rises again
(retrigger), so envelopes restart, and the length resets to the new duration.

**Examples:**
```
3D2.......
..:03C..       ← trigger pulse (duration = .)
```
```
3D2.......
..:03Cg8       ← gate held for 8 ticks
```
The `D` operator writes a `*` bang below itself every 6 ticks (rate 3 × 2). The bang sits next to the `:`, which
fires on the same tick.

### `%` — CV Note + Trigger (mono)

Identical to `:` but sets a mono flag. Currently behaves the same as `:` on Teletype hardware.

### `!` — CV Control Change

Sets a CV output to a voltage without firing a trigger. Requires a neighboring `*` bang.

```
 !0ag
 │││
 ││└── value: 0-z (0-35, scaled to 0–5 V)
 │└─── control number (ignored on Teletype)
 └──── channel: 0-3 → CV output 1-4
```

The control number must be set (any glyph); with `.` there, `!` does nothing.

**Example**: ramp CV output 1 upward, one step per tick (`C` counts 0–y, `D1` bangs every tick):
```
1D1.1Cz
..!00.
```

### `?` — Fine CV

Sets a CV output from two base-36 values, coarse and fine, without a trigger. Requires a neighboring `*` bang.

```
 ?0hg
 │││
 ││└── fine: 0-z
 │└─── coarse: 0-z
 └──── channel: 0-3 → CV output 1-4
```

Value = coarse × 36 + fine, giving 1296 steps (~3.9 mV each) across 0–5 V. Coarse alone moves in steps of ~139 mV.
Unlike `:`, it is not snapped to the scale.

### `&` — CV IN Pitch

Reads the CV IN jack as 1 V/octave pitch every tick and writes it as an octave (below `&`) and a note (below and to
the right), the same form `:` reads. 0–5 V covers octave 0 C to octave 5 C. No bang needed.

```
1D1.&..
..:0...       ← & fills in octave + note after `:0`: CV IN pitch plays on CV/TR 1
```

`&` writes into the cells that are `:`'s octave and note inputs, so an external sequencer or keyboard can drive
ORCA's notes. A voltage sitting between two semitones won't flicker between them. Typed with Shift+7.

## Swing

Hold the **front button** and turn **PARAM** to swing the internal clock, like an MPC 60 / MPC 1000: 50–75 % in
1 % steps, shown on the status line (`SWING 58%`). Pressing the button shows the current value.

ORCA ticks are 16th notes. Swing splits each pair of ticks swing : (100 − swing), so every odd tick comes late:

| Swing | Feel |
|---|---|
| 50 % | Straight |
| 54–58 % | Light shuffle |
| 62 % | Medium |
| 66 % | Triplet feel |
| 71–75 % | Heavy / dotted |

Swing is saved with the scene (**s** in the scene browser), like tempo. It only applies to the internal clock;
on external clock (TR in 1) ticks happen exactly when pulses arrive. The tick counter decides which ticks are odd,
so a reset (TR in 2) puts the next tick back on the beat.

## Scale Quantizer

The **PARAM knob** picks a scale, and every `:` and `%` note snaps to it before it becomes a voltage. Turning the knob
shows the scale's name on the status line. Notes outside the scale move to the nearest scale note; on a tie, the lower one.
The root is C.

| Knob position (left → right) | Scale |
|---|---|
| 1 | Free (no snapping) |
| 2 | Major |
| 3 | Minor |
| 4 | Dorian |
| 5 | Phrygian |
| 6 | Lydian |
| 7 | Mixolydian |
| 8 | Pentatonic minor |
| 9 | Pentatonic major |
| 10 | Harmonic minor |
| 11 | Whole tone |
| 12 | Blues |

### External Trigger Inputs

| Input | Function |
|---|---|
| TR in 1 | **Clock**: each rising edge advances ORCA by one tick (while playing) |
| TR in 2 | **Reset**: a rising edge sets the tick counter back to 0, so `C`, `D`, `U` etc. restart in phase |
| TR in 3–8 | Unused |

While clock pulses arrive on input 1, the internal tempo is muted. It takes over again 2 seconds after the last
pulse. Press Space to start/stop either way.

## Grid Operators

These operators manipulate data on the grid. They form the core logic of ORCA programs.

### Arithmetic

| Op | Name | Inputs | Output | Description |
|---|---|---|---|---|
| `A` | Add | left + right | below | Outputs sum of left and right inputs (mod 36) |
| `B` | Subtract | left + right | below | Outputs absolute difference |
| `L` | Lesser | left + right | below | Outputs the smaller of two values |
| `M` | Multiply | left + right | below | Outputs product (mod 36) |
| `Z` | Lerp | rate(left) + target(right) | below | Moves value toward target at given rate |

For `A`, `B`, `L`, `M`: the left input is one cell left of the operator, the right input is one cell right, and output is one cell below.

```
 3A5
  8    ← output: 3+5=8
```

### Timing & Logic

| Op | Name | Inputs | Output | Description |
|---|---|---|---|---|
| `C` | Clock | rate(left) + mod(right) | below | Outputs `tick / rate % mod` |
| `D` | Delay | rate(left) + mod(right) | below | Outputs `*` bang when `tick % (rate*mod) == 0` |
| `F` | If | left + right | below | Outputs `*` if inputs are equal, `.` otherwise |
| `I` | Increment | rate(left) + mod(right) | below | Counter that increments by rate, wraps at mod |
| `U` | Uclid | steps(left) + max(right) | below | Euclidean rhythm generator |
| `H` | Halt | below | — | Stops the operator below from executing |

**`D` is the primary rhythm generator.** Example — bang every 8 ticks:
```
.1D8
```

**`C` is a counter.** Example — count 0-7 repeating:
```
..C8
```

### Movement

| Op | Name | Description |
|---|---|---|
| `N` | North | Moves up one cell per tick, becomes `*` at wall |
| `E` | East | Moves right one cell per tick |
| `S` | South | Moves down one cell per tick |
| `W` | West | Moves left one cell per tick |

Moving operators carry their character through empty cells. When they hit a non-empty cell or the grid edge, they turn into a `*` bang — useful for triggering output operators.

### Data

| Op | Name | Inputs | Output | Description |
|---|---|---|---|---|
| `G` | Generator | x,y,len (3 left inputs) + data (right) | at (x,y) | Copies a row of data to a target position |
| `K` | Konkat | len(left) + variable names(right) | below | Reads multiple variables at once |
| `O` | Offset | x(left-2), y(left-1) | below | Reads a cell at the given x,y offset |
| `P` | Push | key(left-2), len(left-1) + value(right) | below row | Writes value to position key%len in row below |
| `Q` | Query | x,y,len (3 left inputs) | below | Copies a row of data from source to below |
| `T` | Track | key(left-2), len(left-1) | below | Reads from a data row at position key%len |
| `V` | Variable | name(left) + value(right) | below | Write (if left is set) or read (if right is set) a named variable |
| `X` | Teleport | x(left-2), y(left-1) + value(right) | at (x,y) | Writes a value to an absolute grid position |
| `Y` | Yump | left | rightward | Sends left input rightward through consecutive Y operators |
| `J` | Jump | above | downward | Sends value above downward through consecutive J operators |
| `R` | Random | min(left) + max(right) | below | Random value between min and max |

### Special Characters

| Char | Name | Description |
|---|---|---|
| `.` | Empty | Empty cell, no operation |
| `*` | Bang | Activates neighboring lowercase operators, then disappears |
| `#` | Comment | Locks all cells to the right until the next `#` |
| `;` | UDP | String output (not mapped on Teletype) |
| `=` | OSC | OSC output (not mapped on Teletype) |

## CV Calibration

Each CV output can be trimmed so that 1 V/oct is exact. Open it from the scene browser: **Escape**, then **c**.
Playback stops while the screen is open.

```
CALIBRATE CV 1
> 1V  code 0819
  4V  code 3277
measured: _ mV
```

The selected output (CV 1–4) holds the selected point: **1 V** or **4 V**.

| Key | Action |
|---|---|
| ← / → | Previous / next CV output |
| Tab | Switch between the 1 V and 4 V point |
| ↑ / ↓ | Nudge the point by 1 step (~1.2 mV); with Shift, 10 steps (hold to repeat) |
| 0–9, Backspace | Type the voltage your meter reads, in millivolts |
| Enter | Correct the point by the typed reading (repeat once or twice to refine) |
| r | Reset this output to the uncalibrated values |
| Escape | Save all outputs to flash and return to the grid |

**With a meter:** measure between the CV output and AE ground, ideally with the cable plugged into the module it
will drive. At the 1 V point, type the reading (e.g. `1012`) and press Enter, then repeat until it reads 1000 mV.
Press Tab and do the same at 4 V (target 4000 mV). Repeat for each output.

**By ear / with a tuner:** patch the output into a VCO. At the 1 V point, tune the VCO itself to a C. Press Tab
and nudge the 4 V point with ↑/↓ until the VCO plays the C three octaves higher.

Calibration is stored separately from scenes and survives power cycles. Recalibrate after changing the power
supply or the module's hardware.

## Display

The OLED shows a 25x8 character viewport (5 px cells; 25x7 with the status line) into the 64x64 grid. The viewport scrolls automatically when the cursor approaches the edge (within 2 characters).

**Cell brightness indicates state** (updated as you edit, even while paused):
- Faint dots: empty cells
- Faint `+`: empty cells on every 7th row and column (rulers, like orca-c, which uses 8), so you can see where you are on the
  64x64 grid. They're only drawn: the cell is still `.`
- Bright: operators, values and input cells (read by an operator)
- Grey block: output cells (written by an operator), filling the cell like the cursor
- Faint: comments, including both `#` markers
- Solid block: cursor position (one 5 px cell, same for every glyph); a dimmer block marks the rest of a selection

The levels are the `SHADE_*` constants in `module/main.c` (0x0–0xf).

## Quick Start

1. Power on — you'll see the ORCA grid with a cursor
2. Plug in a USB keyboard
3. Type `D` then move the cursor to build a patch:

```
1D4.......
..:03C....
```

`1D4` bangs every 4 ticks into the cell below it, next to the `:`, which plays a C in octave 3 (3 V) on CV/TR
output 1.

4. Press **Space** to start the clock
5. Connect CV 1 to a VCO's V/Oct input and TR 1 to an envelope gate
