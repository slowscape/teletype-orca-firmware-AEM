#pragma once
#include "base.h"
#include "vmio.h"

void orca_run(Glyph *restrict gbuffer, Mark *restrict mbuffer, Usz height,
              Usz width, Usz tick_number, Oevent_list *oevent_list,
              Usz random_seed);

// Teletype: the CV IN jack as a 1 V/oct note number (octave * 12 + note,
// 0-60), set by the host before orca_run(). Read by the & operator.
extern U8 orca_cv_in_note;
