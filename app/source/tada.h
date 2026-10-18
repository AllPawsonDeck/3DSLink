/* A short "ta-da!" fanfare, synthesised in code (plain C, mono 16-bit). */
#pragma once

#include <stdint.h>

/* Number of samples the fanfare needs at the given sample rate. */
int tada_length(int rate);

/* Writes tada_length(rate) samples into out. */
void tada_render(int16_t *out, int rate);
