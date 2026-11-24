/* The optional SPU output limiter: off it keeps the hardware's 16-bit clamp,
 * on it passes quiet mixes through unchanged (delayed) and keeps loud ones
 * under full scale instead of clipping them. */
#include <psyz.h>
#include <psyz/spu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 8192
#define LOOKAHEAD_DELAY 63
#define SAMPLE_ADDR 0x1080

#define CHECK(condition) do { if (!(condition)) {                            \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1;       \
} } while (0)

static short s_reference[FRAMES * 2];
static short s_output[FRAMES * 2];

/* Two looping ADPCM blocks: 28 samples at +24576, then 28 at -24576. */
static void UploadSquare(void) {
    unsigned char payload[32];
    memset(payload, 0, sizeof(payload));
    payload[1] = 0x04;  /* loop start */
    memset(payload + 2, 0x66, 14);
    payload[17] = 0x03; /* loop end + repeat */
    memset(payload + 18, 0xAA, 14);
    Psyz_SpuMemWrite(SAMPLE_ADDR, payload, sizeof(payload));
}

/* Voices 1.. play the square at the given volume; one voice at full volume
 * is about 3/8 of full scale, so four sum to about 1.5x full scale. */
static void Render(unsigned short volume, int voices, short *out) {
    Psyz_SpuReset(0);
    Psyz_SpuWrite(0x1AA, 0);
    Psyz_SpuWrite(0x184, 0);
    Psyz_SpuWrite(0x186, 0);
    Psyz_SpuWrite(0x18C, 0xFFFF);
    Psyz_SpuWrite(0x18E, 0xFFFF);
    Psyz_SpuWrite(0x198, 0);
    Psyz_SpuWrite(0x19A, 0);
    Psyz_SpuWrite(0x1B0, 0); /* CD volume */
    Psyz_SpuWrite(0x1B2, 0);
    UploadSquare();
    Psyz_SpuWrite(0x1AA, 0x8000 | 0x4000);
    Psyz_SpuWrite(0x180, 0x3FFF);
    Psyz_SpuWrite(0x182, 0x3FFF);
    for (int voice = 1; voice <= voices; voice++) {
        unsigned base = (unsigned)voice << 4;
        Psyz_SpuWrite(base + 0x00, volume);
        Psyz_SpuWrite(base + 0x02, volume);
        Psyz_SpuWrite(base + 0x04, 0x1000);
        Psyz_SpuWrite(base + 0x06, SAMPLE_ADDR >> 3);
        Psyz_SpuWrite(base + 0x08, 0x000F);
        Psyz_SpuWrite(base + 0x0A, 0x1FC0);
        Psyz_SpuWrite(base + 0x0E, SAMPLE_ADDR >> 3);
    }
    Psyz_SpuWrite(0x18C, 0);
    Psyz_SpuWrite(0x18E, 0);
    Psyz_SpuWrite(0x188, (unsigned short)(((1u << voices) - 1) << 1));
    Psyz_SpuPullSamples(out, FRAMES);
}

static int Peak(const short *samples, int count) {
    int peak = 0;
    for (int i = 0; i < count; i++)
        if (abs(samples[i]) > peak) peak = abs(samples[i]);
    return peak;
}

static int ClampedCount(const short *samples, int count) {
    int clamped = 0;
    for (int i = 0; i < count; i++)
        if (samples[i] >= 32767 || samples[i] <= -32768) clamped++;
    return clamped;
}

int main(void) {
    Psyz_SpuInit();

    /* Off is the hardware clamp: the loud mix clips. */
    Psyz_SpuSetOutputLimiter(0);
    Render(0x3FFF, 4, s_reference);
    CHECK(ClampedCount(s_reference, FRAMES * 2) > 1000);

    /* On, the same mix stays under full scale and keeps most of its level. */
    Psyz_SpuSetOutputLimiter(1);
    Render(0x3FFF, 4, s_output);
    CHECK(ClampedCount(s_output, FRAMES * 2) == 0);
    CHECK(Peak(s_output, FRAMES * 2) <= 32000);
    CHECK(Peak(s_output + FRAMES, FRAMES) >= 30000);

    /* A mix that fits is bit-exact, only delayed by the look-ahead. */
    Psyz_SpuSetOutputLimiter(0);
    Render(0x1000, 1, s_reference);
    CHECK(Peak(s_reference, FRAMES * 2) > 2000);
    Psyz_SpuSetOutputLimiter(1);
    Render(0x1000, 1, s_output);
    for (int i = 0; i < LOOKAHEAD_DELAY * 2; i++) CHECK(s_output[i] == 0);
    CHECK(memcmp(s_output + LOOKAHEAD_DELAY * 2, s_reference,
                 (FRAMES - LOOKAHEAD_DELAY) * 2 * sizeof(short)) == 0);

    Psyz_SpuSetOutputLimiter(0);
    puts("spu output limiter tests passed");
    return 0;
}
