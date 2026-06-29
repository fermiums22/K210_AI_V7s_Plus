#include "show.h"
#include "audio.h"
#include "amp.h"
#include "lcd.h"
#include <devices.h>
#include <filesystem.h>
#include <FreeRTOS.h>
#include <task.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

#define SHOW_DIR        "/fs/0"
#define FACE_W          320
#define FACE_H          240
#define FACE_BYTES      (FACE_W * FACE_H * 2)
#define FACE_WORDS      (FACE_W * FACE_H / 2)
#define AUDIO_HZ        16000
#define WIN_MS          40
#define WIN_FRAMES      (AUDIO_HZ * WIN_MS / 1000)
#define WIN_BYTES       (WIN_FRAMES * 4)
#define EMOTION_MS      6000
#define TALK_LEVEL      4

typedef struct {
    const char *name;
} emotion_t;

static const emotion_t emotions[] = {
    { "neutral" },
    { "happy" },
    { "laughing" },
    { "excited" },
    { "love" },
    { "curious" },
    { "surprised" },
    { "sad" },
    { "angry" },
    { "sleepy" },
};

static uint32_t face_closed[FACE_WORDS] __attribute__((aligned(64)));
static uint32_t face_talk[FACE_WORDS] __attribute__((aligned(64)));
static uint8_t env_buf[192];
static int16_t pcm_buf[WIN_FRAMES * 2] __attribute__((aligned(64)));

static int read_all(const char *path, void *dst, int bytes)
{
    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[show] open failed: %s", path);
        return 0;
    }

    uint8_t *p = (uint8_t *)dst;
    int got = 0;
    while (got < bytes) {
        int r = filesystem_file_read(f, p + got, bytes - got);
        if (r <= 0)
            break;
        got += r;
    }
    filesystem_file_close(f);

    if (got != bytes) {
        LOGF("[show] short read %s: %d/%d", path, got, bytes);
        return 0;
    }
    return 1;
}

static int load_faces(const char *emo)
{
    char path[96];
    snprintf(path, sizeof(path), SHOW_DIR "/f_%s.rgb", emo);
    if (!read_all(path, face_closed, FACE_BYTES))
        return 0;

    snprintf(path, sizeof(path), SHOW_DIR "/f_%s_t.rgb", emo);
    if (!read_all(path, face_talk, FACE_BYTES))
        return 0;

    return 1;
}

static int load_env(const char *emo)
{
    char path[96];
    snprintf(path, sizeof(path), SHOW_DIR "/e_%s.env", emo);

    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[show] env missing: %s", path);
        return 0;
    }

    int n = filesystem_file_read(f, env_buf, sizeof(env_buf));
    filesystem_file_close(f);
    if (n <= 0)
        return 0;
    return n;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static handle_t open_wav_pcm16_stereo(const char *emo, int *frames_out)
{
    char path[96];
    snprintf(path, sizeof(path), SHOW_DIR "/e_%s.wav", emo);

    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[show] wav missing: %s", path);
        return 0;
    }

    uint8_t riff[12];
    int r = filesystem_file_read(f, riff, sizeof(riff));
    if (r != (int)sizeof(riff) || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        LOGF("[show] bad wav header: %s", path);
        filesystem_file_close(f);
        return 0;
    }

    int channels = 0;
    int sample_rate = 0;
    int bits = 0;
    int data_bytes = 0;
    for (;;) {
        uint8_t chdr[8];
        r = filesystem_file_read(f, chdr, sizeof(chdr));
        if (r != (int)sizeof(chdr))
            break;

        int size = (int)read_le32(chdr + 4);
        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fmt[32];
            int n = size < (int)sizeof(fmt) ? size : (int)sizeof(fmt);
            if (filesystem_file_read(f, fmt, n) != n)
                break;
            channels = read_le16(fmt + 2);
            sample_rate = (int)read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            if (size > n)
                filesystem_file_set_position(f, filesystem_file_get_position(f) + (size - n));
        } else if (memcmp(chdr, "data", 4) == 0) {
            data_bytes = size;
            break;
        } else {
            filesystem_file_set_position(f, filesystem_file_get_position(f) + size);
        }

        if (size & 1)
            filesystem_file_set_position(f, filesystem_file_get_position(f) + 1);
    }

    if (channels != 2 || sample_rate != AUDIO_HZ || bits != 16 || data_bytes <= 0) {
        LOGF("[show] unsupported wav %s ch=%d hz=%d bits=%d", path, channels, sample_rate, bits);
        filesystem_file_close(f);
        return 0;
    }

    *frames_out = data_bytes / 4;
    return f;
}

static void draw_face(const uint32_t *face, int breath)
{
    if (breath <= 0) {
        lcd_draw_picture(0, 0, FACE_W, FACE_H, (uint32_t *)face);
    } else {
        lcd_draw_rectangle(0, 0, FACE_W - 1, breath - 1, breath, BLACK);
        lcd_draw_picture(0, breath, FACE_W, FACE_H - breath, (uint32_t *)face);
    }
}

static void idle_breath_until(TickType_t until_tick)
{
    int phase = 0;
    amp_set(false);
    while ((int32_t)(xTaskGetTickCount() - until_tick) < 0) {
        int breath = (phase / 3) & 1;
        draw_face(face_closed, breath);
        phase++;
        vTaskDelay(pdMS_TO_TICKS(160));
    }
}

static void play_emotion(const char *emo)
{
    int env_len = load_env(emo);
    int frames_total = 0;
    handle_t wav = open_wav_pcm16_stereo(emo, &frames_total);
    if (!wav || env_len <= 0) {
        idle_breath_until(xTaskGetTickCount() + pdMS_TO_TICKS(EMOTION_MS));
        return;
    }

    int silent_windows = 0;
    int frames_left = frames_total;
    int env_i = 0;
    while (frames_left > 0) {
        int frames = frames_left > WIN_FRAMES ? WIN_FRAMES : frames_left;
        int need = frames * 4;
        int got = filesystem_file_read(wav, (uint8_t *)pcm_buf, need);
        if (got <= 0)
            break;
        frames = got / 4;
        frames_left -= frames;

        int level = env_buf[env_i < env_len ? env_i : env_len - 1];
        int talking = level >= TALK_LEVEL;
        if (talking) {
            silent_windows = 0;
            amp_set(true);
            draw_face((env_i & 1) ? face_talk : face_closed, 0);
        } else {
            silent_windows++;
            if (silent_windows >= 3)
                amp_set(false);
            draw_face(face_closed, ((env_i / 4) & 1));
        }

        audio_write_pcm(pcm_buf, frames);
        env_i++;
    }
    filesystem_file_close(wav);
    amp_set(false);
}

void show_run_forever(void)
{
    int idx = 0;
    for (;;) {
        const char *emo = emotions[idx].name;
        LOGF("[show] emotion=%s", emo);

        TickType_t start = xTaskGetTickCount();
        if (load_faces(emo)) {
            draw_face(face_closed, 0);
            play_emotion(emo);
            idle_breath_until(start + pdMS_TO_TICKS(EMOTION_MS));
        } else {
            lcd_clear(BLACK);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        idx = (idx + 1) % (int)(sizeof(emotions) / sizeof(emotions[0]));
    }
}
