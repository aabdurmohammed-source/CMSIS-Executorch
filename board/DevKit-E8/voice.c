/*---------------------------------------------------------------------------
 * Few-shot keyword spotting, all on the M55, no training data.
 *
 * You teach each word by saying it a few times. Every recording becomes a
 * sequence of MFCC frames (the usual speech features: how loud each pitch band
 * is, 100 times a second). A new recording is compared with every template by
 * dynamic time warping (DTW), which lines the two up even if you say the word
 * faster or slower. The closest word wins, unless it is much farther away than
 * the templates of that word are from each other: then it was not that word.
 *---------------------------------------------------------------------------*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "voice.h"

#define SR          8000
#define FRAME       200     /* 25 ms */
#define HOP         80      /* 10 ms */
#define NFFT        256
#define NBIN        (NFFT / 2 + 1)
#define NMEL        20
#define NCEP        12      /* c1..c12 (c0, the loudness, is left out) */
#define MAX_FR      VOICE_MAX_FRAMES
#define PAD_FR      4       /* frames kept before / after the speech */
#define SKIP_FR     6       /* the joystick click at the start */
#define MIN_SPEECH  12      /* frames: shorter than 120 ms is not a word */
#define WORD_DB     20.0f   /* the word = frames within this of its loudest frame, */
#define GAP_FR      12      /* joined across quiet gaps up to 120 ms ("dan-ce"),   */
#define MAX_WORD    80      /* at most 800 ms: chatter before / after is cut off  */
#define REJECT_K    1.6f    /* reject if farther than this x the word's own spread, */
#define REJECT_MIN  4.0f    /* but never stricter than this                         */
#define REJECT_MAX  6.0f    /* or looser than this (checked with Windows TTS words)  */
#define REJECT_ONE  5.0f    /* while a word has only one template                   */

static float fr_db[1600];                 /* per-frame loudness */
static float query[MAX_FR][NCEP];
static int   query_n;
static float tmpl[VOICE_WORDS][VOICE_TEMPLATES][MAX_FR][NCEP];
static int   tmpl_n[VOICE_WORDS][VOICE_TEMPLATES];
static int   tmpl_count[VOICE_WORDS], tmpl_next[VOICE_WORDS];
static float spread[VOICE_WORDS];         /* mean DTW distance between a word's templates */

static float win[FRAME];
static float cos_t[NFFT / 2], sin_t[NFFT / 2];
static float mel_w[NMEL][NBIN];
static float dct[NCEP][NMEL];
static int   ready;

static float hz_to_mel(float f) { return 2595.0f * log10f(1.0f + f / 700.0f); }
static float mel_to_hz(float m) { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

static void init(void)
{
    const float pi = 3.14159265f;
    for (int i = 0; i < FRAME; ++i) win[i] = 0.54f - 0.46f * cosf(2.0f * pi * (float)i / (FRAME - 1));
    for (int i = 0; i < NFFT / 2; ++i) {
        cos_t[i] = cosf(2.0f * pi * (float)i / NFFT);
        sin_t[i] = -sinf(2.0f * pi * (float)i / NFFT);
    }
    /* triangular mel filters from 100 Hz to 3800 Hz */
    float m0 = hz_to_mel(100.0f), m1 = hz_to_mel(3800.0f), edge[NMEL + 2];
    for (int k = 0; k < NMEL + 2; ++k) edge[k] = mel_to_hz(m0 + (m1 - m0) * (float)k / (NMEL + 1)) * NFFT / SR;
    for (int m = 0; m < NMEL; ++m) {
        for (int b = 0; b < NBIN; ++b) {
            float f = (float)b, w = 0.0f;
            if (f > edge[m] && f <= edge[m + 1]) w = (f - edge[m]) / (edge[m + 1] - edge[m]);
            else if (f > edge[m + 1] && f < edge[m + 2]) w = (edge[m + 2] - f) / (edge[m + 2] - edge[m + 1]);
            mel_w[m][b] = w;
        }
    }
    for (int c = 0; c < NCEP; ++c) {
        for (int m = 0; m < NMEL; ++m) dct[c][m] = cosf(pi * (float)(c + 1) * ((float)m + 0.5f) / NMEL);
    }
    ready = 1;
}

/* in-place radix-2 FFT of 256 complex values */
static void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < NFFT; ++i) {
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int step = NFFT / len;
        for (int i = 0; i < NFFT; i += len) {
            for (int k = 0; k < len / 2; ++k) {
                float wr = cos_t[k * step], wi = sin_t[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr; *ai += ti;
            }
        }
    }
}

static void mfcc_frame(const int16_t *x, float mean, float *out)
{
    static float re[NFFT], im[NFFT];
    float prev = (float)x[0] - mean;
    for (int i = 0; i < NFFT; ++i) {
        if (i < FRAME) {
            float v = (float)x[i] - mean;
            re[i] = (v - 0.97f * prev) * win[i];  /* pre-emphasis, window */
            prev = v;
        } else {
            re[i] = 0.0f;
        }
        im[i] = 0.0f;
    }
    fft(re, im);
    float logmel[NMEL];
    for (int m = 0; m < NMEL; ++m) {
        float e = 0.0f;
        for (int b = 0; b < NBIN; ++b) {
            if (mel_w[m][b] > 0.0f) e += mel_w[m][b] * (re[b] * re[b] + im[b] * im[b]);
        }
        logmel[m] = logf(e + 1.0f);
    }
    for (int c = 0; c < NCEP; ++c) {
        float s = 0.0f;
        for (int m = 0; m < NMEL; ++m) s += dct[c][m] * logmel[m];
        out[c] = s;
    }
}

int voice_features(const int16_t *pcm, int n)
{
    if (!ready) init();
    query_n = 0;
    int nf = (n - FRAME) / HOP + 1;
    if (nf > (int)(sizeof(fr_db) / sizeof(fr_db[0]))) nf = (int)(sizeof(fr_db) / sizeof(fr_db[0]));
    if (nf <= SKIP_FR + MIN_SPEECH) return 0;

    int64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += pcm[i];
    float mean = (float)sum / (float)n;

    /* loudness of each frame; the quiet floor is the average of the quietest fifth */
    float lo[32];
    int nlo = 0;
    float peak = -1e9f;
    int pk = SKIP_FR;
    for (int f = 0; f < nf; ++f) {
        const int16_t *x = pcm + f * HOP;
        float e = 0.0f;
        for (int i = 0; i < FRAME; ++i) { float v = (float)x[i] - mean; e += v * v; }
        fr_db[f] = 10.0f * log10f(e / FRAME + 1.0f);
        if (f < SKIP_FR) continue;
        if (fr_db[f] > peak) { peak = fr_db[f]; pk = f; }
        /* keep the 32 quietest */
        if (nlo < 32) lo[nlo++] = fr_db[f];
        else {
            int mx = 0;
            for (int k = 1; k < 32; ++k) if (lo[k] > lo[mx]) mx = k;
            if (fr_db[f] < lo[mx]) lo[mx] = fr_db[f];
        }
    }
    float floor_db = 0.0f;
    for (int k = 0; k < nlo; ++k) floor_db += lo[k];
    floor_db /= (float)(nlo ? nlo : 1);
    if (peak - floor_db < 10.0f) {
        printf("voice: nothing heard (peak %d dB over the floor)\n", (int)(peak - floor_db));
        return 0;
    }
    /* grow out from the loudest frame: only the burst around it is the word
     * (in a noisy room, voices before / after are quieter than your word) */
    float thr = floor_db + 0.3f * (peak - floor_db);
    if (thr < peak - WORD_DB) thr = peak - WORD_DB;
    int s = pk, e = pk;
    for (int f = pk - 1, gap = 0; f >= SKIP_FR && gap <= GAP_FR; --f) {
        if (fr_db[f] > thr) { s = f; gap = 0; } else ++gap;
    }
    for (int f = pk + 1, gap = 0; f < nf && gap <= GAP_FR; ++f) {
        if (fr_db[f] > thr) { e = f; gap = 0; } else ++gap;
    }
    if (e - s + 1 > MAX_WORD) {  /* keep MAX_WORD frames around the peak */
        s = pk - MAX_WORD / 3 < s ? s : pk - MAX_WORD / 3;
        e = s + MAX_WORD - 1;
    }
    if (e - s + 1 < MIN_SPEECH) {
        printf("voice: too short (%d frames)\n", e - s + 1);
        return 0;
    }
    s = s - PAD_FR < SKIP_FR ? SKIP_FR : s - PAD_FR;
    e = e + PAD_FR >= nf ? nf - 1 : e + PAD_FR;
    if (e - s + 1 > MAX_FR) e = s + MAX_FR - 1;

    for (int f = s; f <= e; ++f) mfcc_frame(pcm + f * HOP, mean, query[query_n++]);
    /* cepstral mean normalisation: takes out the mic and the room */
    for (int c = 0; c < NCEP; ++c) {
        float m = 0.0f;
        for (int f = 0; f < query_n; ++f) m += query[f][c];
        m /= (float)query_n;
        for (int f = 0; f < query_n; ++f) query[f][c] -= m;
    }
    printf("voice: speech %d ms (%d dB over the floor)\n", query_n * 10, (int)(peak - floor_db));
    return query_n;
}

/* DTW distance, normalised by the path length */
static float dtw(const float (*a)[NCEP], int na, const float (*b)[NCEP], int nb)
{
    static float row0[MAX_FR + 1], row1[MAX_FR + 1];
    float *prev = row0, *cur = row1;
    const float inf = 1e30f;
    prev[0] = 0.0f;
    for (int j = 1; j <= nb; ++j) prev[j] = inf;
    for (int i = 1; i <= na; ++i) {
        cur[0] = inf;
        for (int j = 1; j <= nb; ++j) {
            float d = 0.0f;
            for (int c = 0; c < NCEP; ++c) { float t = a[i - 1][c] - b[j - 1][c]; d += t * t; }
            d = sqrtf(d);
            float m = prev[j - 1];
            if (prev[j] < m) m = prev[j];
            if (cur[j - 1] < m) m = cur[j - 1];
            cur[j] = d + m;
        }
        float *t = prev; prev = cur; cur = t;
    }
    return prev[nb] / (float)(na + nb);
}

static void update_spread(int w)
{
    float s = 0.0f;
    int k = 0;
    for (int i = 0; i < tmpl_count[w]; ++i) {
        for (int j = i + 1; j < tmpl_count[w]; ++j) {
            s += dtw((const float (*)[NCEP])tmpl[w][i], tmpl_n[w][i], (const float (*)[NCEP])tmpl[w][j], tmpl_n[w][j]);
            ++k;
        }
    }
    spread[w] = k ? s / (float)k : 0.0f;
}

int voice_add_template(int word)
{
    if (word < 0 || word >= VOICE_WORDS || query_n == 0) return -1;
    int slot = tmpl_next[word];
    memcpy(tmpl[word][slot], query, sizeof(query[0]) * (size_t)query_n);
    tmpl_n[word][slot] = query_n;
    tmpl_next[word] = (slot + 1) % VOICE_TEMPLATES;
    if (tmpl_count[word] < VOICE_TEMPLATES) ++tmpl_count[word];
    update_spread(word);
    printf("voice: word %d has %d template(s), spread %d.%02d\n", word, tmpl_count[word], (int)spread[word],
           (int)(spread[word] * 100.0f) % 100);
    return tmpl_count[word];
}

int voice_count(int word)
{
    return (word >= 0 && word < VOICE_WORDS) ? tmpl_count[word] : 0;
}

int voice_match(void)
{
    if (query_n == 0) return -1;
    float best_d[VOICE_WORDS];
    int best = -1;
    for (int w = 0; w < VOICE_WORDS; ++w) {
        best_d[w] = 1e30f;
        for (int t = 0; t < tmpl_count[w]; ++t) {
            float d = dtw((const float (*)[NCEP])query, query_n, (const float (*)[NCEP])tmpl[w][t], tmpl_n[w][t]);
            if (d < best_d[w]) best_d[w] = d;
        }
        if (tmpl_count[w] && (best < 0 || best_d[w] < best_d[best])) best = w;
    }
    if (best < 0) return -1;
    float limit = tmpl_count[best] >= 2 ? REJECT_K * spread[best] : REJECT_ONE;
    limit = limit < REJECT_MIN ? REJECT_MIN : (limit > REJECT_MAX ? REJECT_MAX : limit);
    printf("voice: distances");
    for (int w = 0; w < VOICE_WORDS; ++w) {
        if (tmpl_count[w]) printf(" w%d=%d.%02d", w, (int)best_d[w], (int)(best_d[w] * 100.0f) % 100);
    }
    printf(", limit %d.%02d -> %s\n", (int)limit, (int)(limit * 100.0f) % 100, best_d[best] <= limit ? "match" : "no match");
    return best_d[best] <= limit ? best : -2;
}
