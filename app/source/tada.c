/*
 * "Ta-da!": a quick pick-up note, then a bright major chord that swells and
 * rings out. Each note is a few harmonics with falling strength (a soft
 * brassy tone), a gentle vibrato on the chord, and smooth fades so nothing
 * clicks.
 */
#include <math.h>
#include <string.h>

#include "tada.h"

#define PI 3.14159265f

typedef struct {
    float start, length; /* seconds */
    float freq[4];       /* up to four notes sounding together */
    int voices;
    float gain;
    float vibrato;       /* depth as a fraction of the pitch */
} note;

static const note notes[] = {
    /* "ta" */
    { 0.00f, 0.13f, { 392.00f }, 1, 0.55f, 0.0f },                          /* G4 */
    /* "da!" */
    { 0.16f, 1.05f, { 523.25f, 659.25f, 783.99f, 1046.50f }, 4, 0.30f, 0.004f }, /* C major */
};

#define TOTAL_SECONDS 1.30f

int tada_length(int rate)
{
    return (int)(TOTAL_SECONDS * (float)rate);
}

/* Attack, short settle, sustain, then an exponential ring-out. */
static float envelope(float t, float length)
{
    const float attack = 0.012f, release = length > 0.5f ? 0.45f : 0.04f;
    if (t < 0 || t > length)
        return 0;
    float e = t < attack ? t / attack : 1.0f - 0.25f * fminf((t - attack) / 0.12f, 1.0f);
    float left = length - t;
    if (left < release)
        e *= expf(-3.5f * (1.0f - left / release)) * (left / release);
    return e;
}

void tada_render(int16_t *out, int rate)
{
    int n = tada_length(rate);
    static const float harmonics[] = { 1.0f, 0.55f, 0.35f, 0.22f, 0.14f, 0.08f };
    const int nh = (int)(sizeof(harmonics) / sizeof(harmonics[0]));

    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)rate, s = 0;
        for (unsigned k = 0; k < sizeof(notes) / sizeof(notes[0]); k++) {
            const note *nt = &notes[k];
            float e = envelope(t - nt->start, nt->length);
            if (e <= 0)
                continue;
            float tt = t - nt->start;
            float vib = 1.0f + nt->vibrato * sinf(2 * PI * 5.5f * tt) * fminf(tt / 0.25f, 1.0f);
            for (int v = 0; v < nt->voices; v++) {
                float f = nt->freq[v] * vib;
                for (int h = 0; h < nh; h++) {
                    if (f * (float)(h + 1) > (float)rate * 0.45f)
                        break; /* stay below Nyquist */
                    s += nt->gain * e * harmonics[h] * sinf(2 * PI * f * (float)(h + 1) * tt);
                }
            }
        }
        s *= 0.42f;
        if (s > 1)
            s = 1;
        if (s < -1)
            s = -1;
        out[i] = (int16_t)(s * 32000.0f);
    }
}
