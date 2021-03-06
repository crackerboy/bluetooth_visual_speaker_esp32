/*
 * fft.c
 *
 *  Created on: 2021-01-14 17:00
 *      Author: Jack Chen <redchenjs@live.com>
 */

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <complex.h>

#include "user/fft.h"

static float freq[FFT_N] = {0.0};

static   int bitrev[FFT_N] = {0.0};
static float window[FFT_N * 2] = {0.0};
static float xscale[BAND_N + 1] = {0.0};
static float complex data[FFT_N] = {0.0};
static float complex root[FFT_N / 2] = {0.0};

static bool generated = false;

static int bit_reverse(int x)
{
    int y = 0;

    for (int i = FFT_N >> 1; i > 0; i >>= 1) {
        y = (y << 1) | (x & 1);
        x >>= 1;
    }

    return y;
}

static void compute_fft_tables(void)
{
    // bit reversal
    for (int i = 0; i < FFT_N; i++) {
        bitrev[i] = bit_reverse(i);
    }

    // hamming window
    for (int i = 0; i < FFT_N * 2; i++) {
        window[i] = 0.53836 - 0.46164 * cosf(i * TWO_PI / (FFT_N * 2 - 1));
    }

    // log xscale
    for (int i = 0; i <= BAND_N; i++) {
        xscale[i] = powf(FFT_N, (float)i / BAND_N) - 0.5;
    }

    // twiddle factor
    for (int i = 0; i < FFT_N / 2; i++) {
        root[i] = cexpf(-i * TWO_PI / (FFT_N - 1) * I);
    }
}

static float compute_freq_lin(const float *freq, int step, int idx)
{
    float n = 0.0;

    for (int i = 0; i < step; i++) {
        n += freq[step * idx + i];
    }

    return n / step * 2.0;
}

static float compute_freq_band(const float *freq, const float *xscale, int band)
{
    float n = 0.0;

    int a = ceilf(xscale[band]);
    int b = floorf(xscale[band + 1]);

    if (b < a) {
        n += freq[b] * (xscale[band + 1] - xscale[band]);
    } else {
        if (a > 0) {
            n += freq[a - 1] * (a - xscale[band]);
        }

        for (; a < b; a++) {
            n += freq[a];
        }

        if (b < FFT_N) {
            n += freq[b] * (xscale[band + 1] - b);
        }
    }

    return 20 * log10f(n * BAND_N / FFT_N / 12.0);
}

void fft_compute_lin(uint16_t *data_out, uint16_t max_val, uint16_t min_val)
{
    for (int i = 0; i < FFT_OUT_N; i++) {
        data_out[i] += compute_freq_lin(freq, FFT_N / FFT_OUT_N, i) * (max_val / 40.0);
        data_out[i] /= 2.0;

        if (data_out[i] > max_val) {
            data_out[i] = max_val;
        } else if (data_out[i] < min_val) {
            data_out[i] = min_val;
        }
    }
}

void fft_compute_log(uint16_t *data_out, uint16_t max_val, uint16_t min_val)
{
    for (int i = 0; i < FFT_OUT_N; i++) {
        data_out[i] += 20 * log10f(1 + compute_freq_lin(freq, FFT_N / FFT_OUT_N, i)) * (max_val / 40.0);
        data_out[i] /= 2.0;

        if (data_out[i] > max_val) {
            data_out[i] = max_val;
        } else if (data_out[i] < min_val) {
            data_out[i] = min_val;
        }
    }
}

void fft_compute_bands(uint16_t *data_out, uint16_t max_val, uint16_t min_val)
{
    static char delay[BAND_N] = {0};

    for (int i = 0; i < BAND_N; i++) {
        float x = (40 + compute_freq_band(freq, xscale, i)) * (max_val / 64.0);

        data_out[i] = MAX(0, data_out[i] - (BAND_FADE - delay[i]));

        if (delay[i]) {
            delay[i]--;
        }

        if (x > data_out[i]) {
            data_out[i]= x;

            delay[i] = BAND_DELAY;
        }

        if (data_out[i] > max_val) {
            data_out[i] = max_val;
        } else if (data_out[i] < min_val) {
            data_out[i] = min_val;
        }
    }
}

void fft_execute(float scale_factor)
{
    int half = 1;

    // Cooley–Tukey algorithm, radix-2 case
    for (int i = FFT_N >> 1; i > 0; i >>= 1) {
        for (int g = 0; g < FFT_N; g += half << 1) {
            for (int b = 0, r = 0; b < half; b++, r += i) {
                float complex even = data[g + b];
                float complex odd  = data[g + b + half] * root[r];

                data[g + b]        = even + odd;
                data[g + b + half] = even - odd;
            }
        }

        half <<= 1;
    }

    // compute the amplitude of each frequency
    for (int i = 0; i < FFT_N / 2; i++) {
        freq[i * 2]     = 0.5 * cabsf(data[i] + conjf(data[FFT_N - 1 - i])) / FFT_N * scale_factor;
        freq[i * 2 + 1] = 0.5 * cabsf(data[i] - conjf(data[FFT_N - 1 - i])) / FFT_N * scale_factor;
    }

    freq[0] /= 2.0;
}

void fft_load_data(const uint8_t *data_in, fft_channel_t channel)
{
    switch (channel) {
        case FFT_CHANNEL_L:
            for (int i = 0; i < FFT_N; i++) {
                int16_t data_re_l = data_in[i * 8 + 1] << 8 | data_in[i * 8];
                int16_t data_im_l = data_in[i * 8 + 5] << 8 | data_in[i * 8 + 4];
                float data_re = data_re_l * window[i * 2];
                float data_im = data_im_l * window[i * 2 + 1];

                data[bitrev[i]] = data_re + data_im * I;
            }
            break;
        case FFT_CHANNEL_R:
            for (int i = 0; i < FFT_N; i++) {
                int16_t data_re_r = data_in[i * 8 + 3] << 8 | data_in[i * 8 + 2];
                int16_t data_im_r = data_in[i * 8 + 7] << 8 | data_in[i * 8 + 6];
                float data_re = data_re_r * window[i * 2];
                float data_im = data_im_r * window[i * 2 + 1];

                data[bitrev[i]] = data_re + data_im * I;
            }
            break;
        case FFT_CHANNEL_LR:
            for (int i = 0; i < FFT_N; i++) {
                int16_t data_re_l = data_in[i * 8 + 1] << 8 | data_in[i * 8];
                int16_t data_re_r = data_in[i * 8 + 3] << 8 | data_in[i * 8 + 2];
                int16_t data_im_l = data_in[i * 8 + 5] << 8 | data_in[i * 8 + 4];
                int16_t data_im_r = data_in[i * 8 + 7] << 8 | data_in[i * 8 + 6];
                float data_re = (data_re_l + data_re_r) / 2.0 * window[i * 2];
                float data_im = (data_im_l + data_im_r) / 2.0 * window[i * 2 + 1];

                data[bitrev[i]] = data_re + data_im * I;
            }
            break;
        default:
            break;
    }
}

void fft_init(void)
{
    memset(data, 0x00, sizeof(data));

    if (!generated) {
        compute_fft_tables();

        generated = true;
    }
}
