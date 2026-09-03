#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp3_decoder_wrapper mp3_decoder_wrapper_t;

mp3_decoder_wrapper_t *mp3_decoder_create(void);
void mp3_decoder_destroy(mp3_decoder_wrapper_t *decoder);
void mp3_decoder_reset(mp3_decoder_wrapper_t *decoder);
int mp3_decoder_decode(mp3_decoder_wrapper_t *decoder,
                       const uint8_t *input, size_t input_len,
                       int16_t *output, size_t output_bytes,
                       size_t *bytes_consumed, size_t *samples_per_channel);
uint32_t mp3_decoder_sample_rate(const mp3_decoder_wrapper_t *decoder);
uint8_t mp3_decoder_channels(const mp3_decoder_wrapper_t *decoder);
uint32_t mp3_decoder_bitrate(const mp3_decoder_wrapper_t *decoder);

#ifdef __cplusplus
}
#endif
