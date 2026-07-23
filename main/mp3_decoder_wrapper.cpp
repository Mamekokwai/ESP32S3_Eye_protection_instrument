#include "mp3_decoder_wrapper.h"

#include <new>
#include "micro_mp3/mp3_decoder.h"

struct mp3_decoder_wrapper {
    micro_mp3::Mp3Decoder decoder;
};

extern "C" mp3_decoder_wrapper_t *mp3_decoder_create(void)
{
    return new (std::nothrow) mp3_decoder_wrapper;
}

extern "C" void mp3_decoder_destroy(mp3_decoder_wrapper_t *decoder)
{
    delete decoder;
}

extern "C" void mp3_decoder_reset(mp3_decoder_wrapper_t *decoder)
{
    if (decoder) {
        decoder->decoder.reset();
    }
}

extern "C" int mp3_decoder_decode(mp3_decoder_wrapper_t *decoder,
                                  const uint8_t *input, size_t input_len,
                                  int16_t *output, size_t output_bytes,
                                  size_t *bytes_consumed, size_t *samples_per_channel)
{
    if (!decoder || !bytes_consumed || !samples_per_channel) {
        return micro_mp3::MP3_INPUT_INVALID;
    }
    return decoder->decoder.decode(input, input_len,
                                   reinterpret_cast<uint8_t *>(output), output_bytes,
                                   *bytes_consumed, *samples_per_channel);
}

extern "C" uint32_t mp3_decoder_sample_rate(const mp3_decoder_wrapper_t *decoder)
{
    return decoder ? decoder->decoder.get_sample_rate() : 0;
}

extern "C" uint8_t mp3_decoder_channels(const mp3_decoder_wrapper_t *decoder)
{
    return decoder ? decoder->decoder.get_channels() : 0;
}

extern "C" uint32_t mp3_decoder_bitrate(const mp3_decoder_wrapper_t *decoder)
{
    return decoder ? decoder->decoder.get_bitrate() : 0;
}
