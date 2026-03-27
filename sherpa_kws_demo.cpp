/**
 * Sherpa-ONNX Keyword Spotting Demo
 *
 * A lightweight wake word detection demo using Sherpa-ONNX KWS
 * Supports both ALSA and Unix socket input modes
 *
 * Features:
 * - ONNX-based neural network keyword spotting
 * - Low latency and CPU usage
 * - Customizable keywords
 * - ALSA or socket audio input
 *
 * Audio format: 16kHz, 16-bit PCM, mono
 */

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <alsa/asoundlib.h>

#include "sherpa-onnx/c-api/c-api.h"

// ─── Configuration ────────────────────────────────────────────────────────────
#define SAMPLE_RATE     16000
#define CHANNELS        1
#define BITS_PER_SAMPLE 16
#define CHUNK_SAMPLES   1600    // 100ms at 16kHz
#define CHUNK_BYTES     (CHUNK_SAMPLES * sizeof(int16_t))

// Audio input modes
enum AudioInputMode {
    MODE_ALSA,      // Read from ALSA device
    MODE_SOCKET     // Read from Unix socket (aec_webrtc output)
};

// Default configuration
static AudioInputMode g_input_mode = MODE_ALSA;
static const char* g_alsa_device   = "hw:1,0";
static const char* g_socket_path   = "/tmp/aec_webrtc";
static const char* g_model_dir     = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01";
static const char* g_keywords_file = "./keywords.txt";
static float       g_threshold     = 0.5f;
static int         g_num_threads   = 2;
static int         g_verbose       = 0;

// ─── State ────────────────────────────────────────────────────────────────────
static volatile bool g_running = true;

// ─── Signal handler ───────────────────────────────────────────────────────────
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[INFO] Caught signal %d, stopping...\n", sig);
    g_running = false;
}

// ─── ALSA Audio Capture ───────────────────────────────────────────────────────
static snd_pcm_t* open_alsa_capture(const char* device) {
    snd_pcm_t* pcm = NULL;
    snd_pcm_hw_params_t* params = NULL;
    int err;

    err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[ERROR] Cannot open audio device '%s': %s\n",
                device, snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);

    snd_pcm_uframes_t period = CHUNK_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0);

    err = snd_pcm_hw_params(pcm, params);
    snd_pcm_hw_params_free(params);

    if (err < 0) {
        fprintf(stderr, "[ERROR] Cannot set audio parameters: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_prepare(pcm);
    printf("[INFO] Audio device '%s' opened: %dHz, %d-bit, %dch\n",
           device, SAMPLE_RATE, BITS_PER_SAMPLE, CHANNELS);
    return pcm;
}

// ─── Socket Connection ────────────────────────────────────────────────────────
static int open_socket_input(const char* socket_path) {
    int sock_fd;
    struct sockaddr_un addr;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[ERROR] Cannot create socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    printf("[INFO] Connecting to socket: %s\n", socket_path);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[ERROR] Cannot connect to socket");
        fprintf(stderr, "       Make sure aec_webrtc is running with -o %s\n", socket_path);
        close(sock_fd);
        return -1;
    }

    printf("[INFO] Socket connected successfully\n");
    return sock_fd;
}

// ─── Usage ────────────────────────────────────────────────────────────────────
static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nAudio Input Modes:\n");
    printf("  -d <device>   ALSA mode: capture from device (default: %s)\n", g_alsa_device);
    printf("  -i <socket>   Socket mode: read from Unix socket (default: %s)\n", g_socket_path);
    printf("                Use this to read AEC-processed audio from aec_webrtc\n");
    printf("\nKWS Options:\n");
    printf("  -m <dir>      Model directory (default: %s)\n", g_model_dir);
    printf("  -k <file>     Keywords file (default: %s)\n", g_keywords_file);
    printf("  -t <thresh>   Detection threshold 0.0~1.0 (default: %.2f)\n", g_threshold);
    printf("  -n <threads>  Number of threads (default: %d)\n", g_num_threads);
    printf("  -v            Verbose output\n");
    printf("  -h            Show this help\n");
    printf("\nExamples:\n");
    printf("  # ALSA mode (direct microphone)\n");
    printf("  %s -d hw:1,0 -m ./sherpa-onnx-kws-model -k keywords.txt\n", prog);
    printf("\n  # Socket mode (AEC-processed audio from aec_webrtc)\n");
    printf("  %s -i /tmp/aec_webrtc -m ./sherpa-onnx-kws-model -k keywords.txt\n", prog);
    printf("\nKeywords file format (one per line):\n");
    printf("  小爱同学\n");
    printf("  你好小爱\n");
    printf("  嗨小爱\n");
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "d:i:m:k:t:n:vh")) != -1) {
        switch (opt) {
            case 'd':
                g_input_mode = MODE_ALSA;
                g_alsa_device = optarg;
                break;
            case 'i':
                g_input_mode = MODE_SOCKET;
                g_socket_path = optarg;
                break;
            case 'm': g_model_dir     = optarg; break;
            case 'k': g_keywords_file = optarg; break;
            case 't': g_threshold     = atof(optarg); break;
            case 'n': g_num_threads   = atoi(optarg); break;
            case 'v': g_verbose       = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║     Sherpa-ONNX Keyword Spotting Demo           ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    if (g_input_mode == MODE_ALSA) {
        printf("[INFO] Input mode:   ALSA\n");
        printf("[INFO] ALSA device:  %s\n", g_alsa_device);
    } else {
        printf("[INFO] Input mode:   Socket\n");
        printf("[INFO] Socket path:  %s\n", g_socket_path);
    }
    printf("[INFO] Model dir:    %s\n", g_model_dir);
    printf("[INFO] Keywords:     %s\n", g_keywords_file);
    printf("[INFO] Threshold:    %.2f\n", g_threshold);
    printf("[INFO] Threads:      %d\n", g_num_threads);
    printf("\n");

    // Install signal handlers
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize Sherpa-ONNX KWS
    SherpaOnnxKeywordSpotterConfig config;
    memset(&config, 0, sizeof(config));

    // Model configuration
    config.model_config.transducer.encoder = "";
    config.model_config.transducer.decoder = "";
    config.model_config.transducer.joiner = "";
    config.model_config.paraformer.encoder = "";
    config.model_config.paraformer.decoder = "";
    config.model_config.zipformer2_ctc.model = "";
    config.model_config.nemo_ctc.model = "";

    // For KWS, we typically use a specific model path
    char encoder_path[512];
    char decoder_path[512];
    char joiner_path[512];
    char tokens_path[512];

    snprintf(encoder_path, sizeof(encoder_path), "%s/encoder-epoch-12-avg-2-chunk-16-left-64.onnx", g_model_dir);
    snprintf(decoder_path, sizeof(decoder_path), "%s/decoder-epoch-12-avg-2-chunk-16-left-64.onnx", g_model_dir);
    snprintf(joiner_path, sizeof(joiner_path), "%s/joiner-epoch-12-avg-2-chunk-16-left-64.onnx", g_model_dir);
    snprintf(tokens_path, sizeof(tokens_path), "%s/tokens.txt", g_model_dir);
    config.model_config.transducer.encoder = encoder_path;
    config.model_config.transducer.decoder = decoder_path;
    config.model_config.transducer.joiner = joiner_path;
    config.model_config.tokens = tokens_path;
    config.model_config.num_threads = g_num_threads;
    config.model_config.provider = "cpu";
    config.model_config.debug = g_verbose;

    // KWS-specific configuration
    config.max_active_paths = 4;
    config.num_trailing_blanks = 1;
    config.keywords_score = g_threshold;
    config.keywords_threshold = g_threshold;
    config.keywords_file = g_keywords_file;

    printf("[INFO] Initializing Sherpa-ONNX KWS...\n");
    // SherpaOnnxKeywordSpotter* kws = SherpaOnnxCreateKeywordSpotter(&config);
    const SherpaOnnxKeywordSpotter* kws = SherpaOnnxCreateKeywordSpotter(&config);
    if (!kws) {
        fprintf(stderr, "[ERROR] Failed to create keyword spotter\n");
        fprintf(stderr, "  Check model files in: %s\n", g_model_dir);
        fprintf(stderr, "  Check keywords file: %s\n", g_keywords_file);
        return 1;
    }
    printf("[INFO] KWS initialized successfully\n");

    // Create online stream
    // SherpaOnnxOnlineStream* stream = SherpaOnnxCreateKeywordStream(kws);
    const SherpaOnnxOnlineStream* stream = SherpaOnnxCreateKeywordStream(kws);
    if (!stream) {
        fprintf(stderr, "[ERROR] Failed to create online stream\n");
        SherpaOnnxDestroyKeywordSpotter(kws);
        return 1;
    }

    // Open audio input (ALSA or Socket)
    snd_pcm_t* pcm = NULL;
    int sock_fd = -1;

    if (g_input_mode == MODE_ALSA) {
        pcm = open_alsa_capture(g_alsa_device);
        if (!pcm) {
            fprintf(stderr, "[ERROR] Failed to open ALSA device\n");
            SherpaOnnxDestroyOnlineStream(stream);
            SherpaOnnxDestroyKeywordSpotter(kws);
            return 1;
        }
    } else {
        sock_fd = open_socket_input(g_socket_path);
        if (sock_fd < 0) {
            fprintf(stderr, "[ERROR] Failed to connect to socket\n");
            SherpaOnnxDestroyOnlineStream(stream);
            SherpaOnnxDestroyKeywordSpotter(kws);
            return 1;
        }
    }

    printf("[INFO] Listening for keywords...\n");
    printf("[INFO] Press Ctrl+C to stop\n\n");

    // Audio processing loop
    int16_t audio_buf[CHUNK_SAMPLES];
    float audio_float[CHUNK_SAMPLES];
    long frame_count = 0;

    while (g_running) {
        int samples_read = 0;

        if (g_input_mode == MODE_ALSA) {
            // Read from ALSA
            snd_pcm_sframes_t frames = snd_pcm_readi(pcm, audio_buf, CHUNK_SAMPLES);

            if (frames < 0) {
                if (frames == -EPIPE) {
                    snd_pcm_prepare(pcm);
                    continue;
                } else if (frames == -EAGAIN) {
                    usleep(1000);
                    continue;
                } else {
                    fprintf(stderr, "[ERROR] ALSA read error: %s\n", snd_strerror(frames));
                    break;
                }
            }

            if (frames == 0) {
                usleep(1000);
                continue;
            }

            samples_read = (int)frames;
        } else {
            // Read from socket
            ssize_t n = read(sock_fd, audio_buf, CHUNK_BYTES);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("[ERROR] Socket read error");
                break;
            } else if (n == 0) {
                fprintf(stderr, "[ERROR] Socket closed by aec_webrtc\n");
                break;
            }

            samples_read = (int)(n / sizeof(int16_t));
        }

        // Convert int16 to float32 (normalized to [-1, 1])
        for (int i = 0; i < samples_read; i++) {
            audio_float[i] = audio_buf[i] / 32768.0f;
        }

        // Feed audio to KWS
        SherpaOnnxOnlineStreamAcceptWaveform(stream, SAMPLE_RATE, audio_float, samples_read);

        // Check if keyword is detected
        while (SherpaOnnxIsKeywordStreamReady(kws, stream)) {
            SherpaOnnxDecodeKeywordStream(kws, stream);
        }

        // const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResultAsJson(kws, stream);
        const char* result =  SherpaOnnxGetKeywordResultAsJson(kws, stream);
        if (result) {
            printf("%s\n", result);
            SherpaOnnxFreeKeywordResultJson(result);
        }
        // if (result && result->keyword && strlen(result->keyword) > 0) {
        //     printf("\n");
        //     printf("╔══════════════════════════════════════╗\n");
        //     printf("║       KEYWORD DETECTED!              ║\n");
        //     printf("╚══════════════════════════════════════╝\n");
        //     printf("[KWS] Keyword: %s\n", result->keyword);
        //     printf("[KWS] JSON: %s\n", result->json);
        //     printf("\n");

        //     SherpaOnnxDestroyKeywordResult(result);
        // }

        frame_count++;

        // Print activity indicator every ~2 seconds
        if (frame_count % 20 == 0) {
            printf("\r[LISTENING] Frames: %ld | Threshold: %.2f  ", frame_count, g_threshold);
            fflush(stdout);
        }
    }

    // Cleanup
    printf("\n[INFO] Stopping...\n");

    if (pcm) {
        snd_pcm_close(pcm);
    }
    if (sock_fd >= 0) {
        close(sock_fd);
    }

    SherpaOnnxDestroyOnlineStream(stream);
    SherpaOnnxDestroyKeywordSpotter(kws);

    printf("[INFO] Done.\n");
    return 0;
}