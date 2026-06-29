/*
 * Audio out — I2S0 TX → PT8211 DAC → PAM8403 amp.
 *
 * Pins (see pinout.h):  WS=IO33, DATA=IO34 (I2S0_OUT_D1, NOT D0!), BCK=IO35.
 * The DAC sits on TX data pad D1, so the render channel mask is 0x0C:
 * i2s.cpp walks the mask two bits per pad (bits[1:0]=D0, bits[3:2]=D1, ...).
 */
#include "audio.h"
#include "amp.h"
#include "pinout.h"
#include <devices.h>
#include <fpioa.h>
#include <FreeRTOS.h>
#include <math.h>
#include <stdint.h>

static handle_t i2s;

#define SR   AUD_SAMPLE_HZ          /* 16000 */
#define PAD_D1_MASK   (3u << 2)     /* enable TX channel pair on pad D1 */

void audio_init(void)
{
    fpioa_set_function(PIN_AUD_BCK,  FUNC_I2S0_SCLK);
    fpioa_set_function(PIN_AUD_WS,   FUNC_I2S0_WS);
    fpioa_set_function(PIN_AUD_DATA, FUNC_I2S0_OUT_D1);

    i2s = io_open("/dev/i2s0");
    configASSERT(i2s);

    audio_format_t fmt = { AUDIO_FMT_PCM, 16, SR, 2 };
    /* I2S_AM_RIGHT: PT8211 is a right-justified ("LSB") 16-bit DAC.
     * If a tone sounds like harsh noise, try I2S_AM_STANDARD / I2S_AM_LEFT. */
    i2s_config_as_render(i2s, &fmt, 200, I2S_AM_RIGHT, PAD_D1_MASK);
    i2s_start(i2s);
}

void audio_tone(int freq_hz, int ms, int amplitude)
{
    enum { N = 512 };                /* frames per chunk */
    static int16_t buf[N * 2];       /* stereo interleaved */

    int total = (int)((long long)SR * ms / 1000);
    double ph = 0.0, dph = 2.0 * M_PI * freq_hz / SR;

    amp_set(true);
    for (int sent = 0; sent < total; ) {
        int n = (total - sent) < N ? (total - sent) : N;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(amplitude * sin(ph));
            ph += dph;
            if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
            buf[2 * i]     = s;
            buf[2 * i + 1] = s;
        }
        io_write(i2s, (const uint8_t *)buf, n * 4);
        sent += n;
    }
}

void audio_write_pcm(const int16_t *stereo_samples, int frames)
{
    if (frames > 0)
        io_write(i2s, (const uint8_t *)stereo_samples, frames * 4);
}

void audio_test(void)
{
    audio_tone(784,  180, 9000);   /* G5 */
    audio_tone(988,  180, 9000);   /* B5 */
    audio_tone(1175, 280, 9000);   /* D6 */
    amp_set(false);                /* silence the amp when idle */
}
