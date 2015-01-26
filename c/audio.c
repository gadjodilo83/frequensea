// Play random noise as audio

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OpenAL/al.h>
#include <OpenAL/alc.h>

#include <libhackrf/hackrf.h>
#include <sndfile.h>

const int IN_SAMPLE_RATE = 2e6;
const int OUT_SAMPLE_RATE = 48000;

typedef struct {
    float x;
    float y;
} vec2;

#define HACKRF_CHECK_STATUS(device, status, message) \
if (status != 0) { \
    fprintf(stderr, "HackRF error: %s\n", message); \
    if (device != NULL) hackrf_close(device); \
    hackrf_exit(); \
    exit(EXIT_FAILURE); \
} \

void nal_check_error(const char *file, int line) {
    ALenum err = alGetError();
    int has_error = 0;
    while (err != AL_NO_ERROR) {
        has_error = 1;
        char *msg = NULL;
        switch (err) {
            case AL_INVALID_NAME:
                msg = "AL_INVALID_NAME";
                break;
            case AL_INVALID_ENUM:
                msg = "AL_INVALID_ENUM";
                break;
            case AL_INVALID_VALUE:
                msg = "AL_INVALID_VALUE";
                break;
            case AL_INVALID_OPERATION:
                msg = "AL_INVALID_OPERATION";
                break;
            case AL_OUT_OF_MEMORY:
                msg = "AL_OUT_OF_MEMORY";
                break;
        }
        fprintf(stderr, "OpenAL error: %s - %s:%d\n", msg, file, line);
        err = alGetError();
    }
    if (has_error) {
        exit(EXIT_FAILURE);
    }
}

#define NAL_CHECK_ERROR() nal_check_error(__FILE__, __LINE__)

ALuint buffer;
ALenum format = AL_FORMAT_MONO16;
const ALsizei freq = OUT_SAMPLE_RATE;
const ALsizei size = freq * 3 * sizeof(format);
ALuint source;

ALshort *audio_buffer_samples;
int has_received = 0;

int received_size = 0;
vec2 *vec_buffer;

const int HACKRF_SAMPLES_SIZE = 131072;
const int HACKRF_BUFFER_SIZE = 262144;

const float TAU = M_PI * 2;

const int desired_freq = 100.9e6;
const int center_freq = desired_freq + 100000;

// Generates coefficients for a FIR low-pass filter with the given
// half-amplitude frequency and kernel length at the given sample rate.
//
// sample_rate    - The signal's sample rate.
// half_ampl_freq - The half-amplitude frequency in Hz.
// length         - The filter kernel's length. Should be an odd number.
//
// Returns the FIR coefficients for the filter.
float *get_low_pass_fir_coefficients(int sample_rate, int half_ampl_freq, int length) {
    length += (length + 1) % 2;
    float freq = half_ampl_freq / (float) sample_rate;
    float *coefs = calloc(length, sizeof(float));
    int center = floor(length / 2);
    float sum = 0;
    for (int i = 0; i < length; i++) {
        float val;
        if (i == center) {
            val = TAU * freq;
        } else {
            float angle = TAU * (i + 1) / (float) (length + 1);
            val = sin(TAU * freq * (i - center)) / (float) (i - center);
            val *= 0.42 - 0.5 * cos(angle) + 0.08 * cos(2 * angle);
        }
        sum += val;
        coefs[i] = val;
    }
    for (int i = 0; i < length; i++) {
        coefs[i] /= sum;
    }
    return coefs;
}

typedef struct {
    int length;
    float *coefficients;
    int offset;
    int center;
    int samples_length;
    float *samples;
} fir_filter;

fir_filter *fir_filter_new(int sample_rate, int half_ampl_freq, int length) {
    fir_filter *filter = calloc(1, sizeof(fir_filter));
    filter->length = length;
    filter->coefficients = get_low_pass_fir_coefficients(sample_rate, half_ampl_freq, length);
    filter->offset = length - 1;
    filter->center = floor(length / 2);
    filter->samples_length = filter->offset;
    filter->samples = calloc(filter->samples_length, sizeof(float));
    return filter;
}

void fir_filter_load(fir_filter *filter, float *samples, int length) {
    float *new_samples = calloc(length + filter->offset, sizeof(float));
    int o = (filter->samples_length - filter->offset);
    memcpy(new_samples, filter->samples + o, filter->offset * sizeof(float));
    memcpy(new_samples + filter->offset, samples, length - filter->offset * sizeof(float));
    free(filter->samples);
    filter->samples_length = length + filter->offset;
    //printf("%d\n", filter->samples_length);
    filter->samples = new_samples;
}

float fir_filter_get(fir_filter *filter, int index) {
    float v = 0;
    for (int i = 0; i < filter->length; i++) {
        v += filter->coefficients[i] * filter->samples[index + i];
    }
    return v;
}

void fir_filter_free(fir_filter *filter) {
    free(filter->coefficients);
    free(filter->samples);
    free(filter);
}

typedef struct {
    int in_rate;
    int out_rate;
    fir_filter *filter;
    float rate_mul;
    int out_length;
    float *out_samples;
} downsampler;

downsampler *downsampler_new(int in_rate, int out_rate, int in_length, int filter_freq, int kernel_length) {
    downsampler *d = calloc(1, sizeof(downsampler));
    d->in_rate = in_rate;
    d->out_rate = out_rate;
    d->filter = fir_filter_new(in_rate, filter_freq, kernel_length);
    d->rate_mul = in_rate / (float) out_rate;
    // Fixme: remove HACKRF_SAMPLES_SIZE
    d->out_length = floor(in_length / d->rate_mul);
    d->out_samples = calloc(d->out_length, sizeof(float));
    return d;
}

void downsampler_process(downsampler *d, float *samples, int length) {
    fir_filter_load(d->filter, samples, length);
    float t = 0;
    for (int i = 0; i < d->out_length; i++) {
        d->out_samples[i] = fir_filter_get(d->filter, floor(t));
        t += d->rate_mul;
    }
}

void downsampler_free(downsampler *d) {
    fir_filter_free(d->filter);
    free(d->out_samples);
    free(d);
}

float *samples_i;
float *samples_q;

float cosine = 1;
float sine = 0;

downsampler *downsampler_i;
downsampler *downsampler_q;
downsampler *downsampler_audio;

float l_i = 0;
float l_q = 0;

float average(float *values, int length) {
    float sum = 0;
    for (int i = 0; i < length; i++) {
        sum += values[i];
    }
    return sum / length;
}

void shift_frequency(float *samples_i, float *samples_q, int length, int freq_offset, int sample_rate, float *cosine_ptr, float *sine_ptr) {
    float delta_cos = cos(TAU * freq_offset / (float) sample_rate);
    float delta_sin = sin(TAU * freq_offset / (float) sample_rate);
    float cosine = *cosine_ptr;
    float sine = *sine_ptr;
    for (int i = 0; i < length; i++) {
        float vi = samples_i[i];
        float vq = samples_q[i];
        samples_i[i] = vi * cosine - vq * sine;
        samples_q[i] = vi * sine + vq * cosine;
        float new_sine = cosine * delta_sin + sine * delta_cos;
        float new_cosine = cosine * delta_cos - sine * delta_sin;
        sine = new_sine;
        cosine = new_cosine;
    }
    *cosine_ptr = cosine;
    *sine_ptr = sine;
}

int receive_sample_block(hackrf_transfer *transfer) {
    if (received_size > size) return 0;

    // Convert to floats
    for (int i = 0; i < HACKRF_SAMPLES_SIZE; i++) {
        unsigned int vi = transfer->buffer[i * 2];
        unsigned int vq = transfer->buffer[i * 2 + 1];
        samples_i[i] = vi / 128.0 - 0.995;
        samples_q[i] = vq / 128.0 - 0.995;
        //samples_i[i] = (vi - 128.0) / 256.0;
        //samples_q[i] = (vq - 128.0) / 256.0;
    }

    // Shift frequency
    int freq_offset = center_freq - desired_freq;
    shift_frequency(samples_i, samples_q, HACKRF_SAMPLES_SIZE, freq_offset, IN_SAMPLE_RATE, &cosine, &sine);

    // Downsample
    downsampler_process(downsampler_i, samples_i, HACKRF_SAMPLES_SIZE);
    downsampler_process(downsampler_q, samples_q, HACKRF_SAMPLES_SIZE);

    // Demodulate
    int out_length = downsampler_i->out_length;
    float *demodulated = calloc(out_length, sizeof(float));

    float prev = 0;
    float difSqrSum = 0;

    float AMPL_CONV = 336000 / (2 * M_PI * 75000);

    for (int i = 0; i < out_length; i++) {
        float real = l_i * downsampler_i->out_samples[i] + l_q * downsampler_q->out_samples[i];
        float imag = l_i * downsampler_q->out_samples[i] - downsampler_i->out_samples[i] * l_q;
        float sgn = 1;
        if (imag < 0) {
            sgn *= -1;
            imag *= -1;
        }
        float ang = 0;
        float div;
        if (real == imag) {
            div = 1;
        } else if (real > imag) {
            div = imag / real;
        } else {
            ang = -M_PI / 2;
            div = real / imag;
            sgn *= -1;
        }
        demodulated[i] = sgn *
            (ang + div
                / (0.98419158358617365
                    + div * (0.093485702629671305
                        + div * 0.19556307900617517))) * AMPL_CONV;
        l_i = downsampler_i->out_samples[i];
        l_q = downsampler_q->out_samples[i];
        float dif = prev - demodulated[i];
        difSqrSum += dif * dif;
        prev = demodulated[i];
    }

    // Downsample again
    downsampler_process(downsampler_audio, demodulated, out_length);
    for (int i = 0; i < out_length; i++) {
        float f_sample = downsampler_audio->out_samples[i];
        int16_t i_sample = f_sample * 1000;
        // if (i == 0) {
        //     printf("%f %f %d\n", downsampler_audio->out_samples[i], f_sample, i_sample);

        // }

        audio_buffer_samples[received_size++] = i_sample;
    }


    //received_size += out_length;
    printf("Received %.1f%%\n", received_size / (float)size * 100);

    if (received_size > size) {
        FILE *fp = fopen("out.raw", "w");
        fwrite(audio_buffer_samples, sizeof(int16_t), size, fp);
        fclose(fp);


        alBufferData(buffer, format, audio_buffer_samples, size, freq);
        NAL_CHECK_ERROR();
        alSourcei(source, AL_BUFFER, buffer);
        NAL_CHECK_ERROR();
        alSourcePlay(source);
        NAL_CHECK_ERROR();

    }
    return 0;
}

int main() {
    int status;
    hackrf_device *hrf;

    audio_buffer_samples = calloc(size + freq, sizeof(ALshort)); // We need a bit of extra data because we go over the buffer size.
    vec_buffer = calloc(HACKRF_BUFFER_SIZE, sizeof(vec2));

    // Initialize the audio context
    ALCdevice *device = alcOpenDevice(NULL);
    if (!device) {
        fprintf(stderr, "Could not open audio device.\n");
        return 1;
    }
    ALCcontext *ctx = alcCreateContext(device, NULL);
    alcMakeContextCurrent(ctx);

    // Initialize an audio buffer
    alGetError(); // clear error code
    alGenBuffers(1, &buffer);
    NAL_CHECK_ERROR();

    samples_i = calloc(HACKRF_SAMPLES_SIZE, sizeof(float));
    samples_q = calloc(HACKRF_SAMPLES_SIZE, sizeof(float));

    const int INTER_RATE = 336000;
    const int  MAX_F = 75000;
    const float FILTER = MAX_F * 0.8;


//   var demodulator = new FMDemodulator(inRate, INTER_RATE, MAX_F, FILTER, 51);
// function FMDemodulator(inRate, outRate, maxF, filterFreq, kernelLen) {
//   var coefs = getLowPassFIRCoeffs(inRate, filterFreq, kernelLen);
//       var downsamplerI = new Downsampler(inRate, outRate, coefs);



    int filter_freq = FILTER * 0.8;
    downsampler_i = downsampler_new(IN_SAMPLE_RATE, INTER_RATE, HACKRF_SAMPLES_SIZE, filter_freq, 51);
    downsampler_q = downsampler_new(IN_SAMPLE_RATE, INTER_RATE, HACKRF_SAMPLES_SIZE, filter_freq, 51);

    downsampler_audio = downsampler_new(INTER_RATE, OUT_SAMPLE_RATE, downsampler_i->out_length, 10000, 41);

    // Fill buffer with random data
    //arc4random_buf(data, size);

    // Initialize a source
    alGenSources(1, &source);
    NAL_CHECK_ERROR();

    // Attach buffer to source
    //alSourcei(source, AL_BUFFER, buffer);
    //NAL_CHECK_ERROR();

    // Play
    //alSourcePlay(source);
    //NAL_CHECK_ERROR();

    status = hackrf_init();
    HACKRF_CHECK_STATUS(NULL, status, "hackrf_init");

    status = hackrf_open(&hrf);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_open");

    status = hackrf_set_freq(hrf, center_freq);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_set_freq");

    status = hackrf_set_sample_rate(hrf, IN_SAMPLE_RATE);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_set_sample_rate");

    status = hackrf_set_amp_enable(hrf, 0);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_set_amp_enable");

    status = hackrf_set_lna_gain(hrf, 32);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_set_lna_gain");

    status = hackrf_set_vga_gain(hrf, 30);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_set_lna_gain");

    status = hackrf_start_rx(hrf, receive_sample_block, NULL);
    HACKRF_CHECK_STATUS(hrf, status, "hackrf_start_rx");


    // Playing is asynchronous so wait a while
    sleep(3 * 2);

    // Cleanup
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(device);

    hackrf_stop_rx(hrf);
    hackrf_close(hrf);
    hackrf_exit();
    return 0;
}
