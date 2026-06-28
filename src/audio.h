#pragma once

/* I2S0 → PT8211 DAC → PAM8403. Mono tone test path.
 * audio_init() must run after amp_init() (amp starts muted). */
void audio_init(void);
void audio_tone(int freq_hz, int ms, int amplitude);
void audio_test(void);   /* short 3-note chime, then mutes the amp */
