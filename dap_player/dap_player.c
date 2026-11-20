#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <math.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include <tinyalsa/pcm.h>

#define PORT 8888
#define DAC_RATE 48000
#define DAC_CHANNELS 2
#define BUFFER_FRAMES 1024
#define MAX_TRACKS 1024
#define MAX_PATH 512

typedef enum {
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} PlaybackState;

typedef struct {
    char path[MAX_PATH];
    char filename[128];
} Track;

typedef struct {
    Track tracks[MAX_TRACKS];
    int count;
    int current_index;
    PlaybackState state;
    int volume; // 0 - 100
    int shuffle;
    int tape_stop_requested;
    int spin_up_active;
    double tape_speed; // 1.0 = normal, 0.0 = stopped
    uint64_t current_pos_frames;
    uint64_t total_frames;
    uint32_t sample_rate;
    uint32_t channels;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} PlayerContext;

static PlayerContext ctx;

void playlist_clear() {
    pthread_mutex_lock(&ctx.lock);
    ctx.count = 0;
    ctx.current_index = -1;
    pthread_mutex_unlock(&ctx.lock);
}

void playlist_add(const char *path) {
    pthread_mutex_lock(&ctx.lock);
    if (ctx.count < MAX_TRACKS) {
        strncpy(ctx.tracks[ctx.count].path, path, MAX_PATH - 1);
        const char *slash = strrchr(path, '/');
        strncpy(ctx.tracks[ctx.count].filename, slash ? slash + 1 : path, 127);
        ctx.count++;
    }
    pthread_mutex_unlock(&ctx.lock);
}

void scan_directory(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        if (strcasecmp(dir->d_name, "System Volume Information") == 0) continue;
        if (strcasecmp(dir->d_name, "modules") == 0) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir->d_name);

        const char *ext = strrchr(dir->d_name, '.');
        if (ext) {
            if (strcasecmp(ext, ".flac") == 0 || strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0) {
                playlist_add(full_path);
                continue;
            }
        }

        // Safely check if directory without depending on 32-bit stat()
        DIR *subdir = opendir(full_path);
        if (subdir) {
            closedir(subdir);
            scan_directory(full_path);
        }
    }
    closedir(d);
}

void init_hardware_and_storage() {
    system("mount -o remount,rw,exec /mnt/UDISK 2>/dev/null");
    system("echo usb_host > /sys/devices/platform/soc@2002000/soc@2002000:usbc0@0/otg_role 2>/dev/null");
    system("cat /sys/devices/platform/soc@2002000/soc@2002000:usbc0@0/usb_host >/dev/null 2>&1");

    // 1. Load SCSI storage drivers from flash
    system("insmod /mnt/UDISK/modules/v821_compat.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/scsi_mod.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/sd_mod.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/usb-storage.ko 2>/dev/null");

    // 2. Mount USB storage if not accessible yet
    if (access("/mnt/exUDISK/Music", F_OK) != 0) {
        system("mkdir -p /mnt/exUDISK");
        for (int i = 0; i < 6; i++) {
            for (char l = 'a'; l <= 'e'; l++) {
                char dev[32];
                snprintf(dev, sizeof(dev), "/dev/sd%c1", l);
                if (access(dev, F_OK) == 0) {
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd), "mount -t vfat -o ro %s /mnt/exUDISK 2>/dev/null || mount -o ro %s /mnt/exUDISK 2>/dev/null", dev, dev);
                    system(cmd);
                    if (access("/mnt/exUDISK/Music", F_OK) == 0) break;
                    system("umount -f /mnt/exUDISK 2>/dev/null");
                }
            }
            if (access("/mnt/exUDISK/Music", F_OK) == 0) break;
            usleep(300000);
        }
    }

    // 3. Load Audio drivers from USB storage (ALWAYS executed, never skipped!)
    system("insmod /mnt/exUDISK/modules/soundcore.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-timer.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-pcm.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-hwdep.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-rawmidi.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-usbmidi-lib.ko 2>/dev/null");
    system("insmod /mnt/exUDISK/modules/snd-usb-audio.ko 2>/dev/null");
}

int get_next_index() {
    if (ctx.count == 0) return -1;
    if (ctx.shuffle) {
        return rand() % ctx.count;
    }
    return (ctx.current_index + 1) % ctx.count;
}

int get_prev_index() {
    if (ctx.count == 0) return -1;
    if (ctx.shuffle) {
        return rand() % ctx.count;
    }
    return (ctx.current_index - 1 + ctx.count) % ctx.count;
}

// Tape-stop DSP apply to 16-bit interleaved stereo buffer
void apply_tape_stop_and_volume(int16_t *buf, int frames, int channels, double speed, int volume) {
    float vol = (float)volume / 100.0f;
    for (int i = 0; i < frames; i++) {
        float factor = vol * (float)(speed * speed);
        for (int c = 0; c < channels; c++) {
            float s = (float)buf[i * channels + c] * factor;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            buf[i * channels + c] = (int16_t)s;
        }
    }
}

void *audio_playback_thread(void *arg) {
    struct pcm_config config;
    memset(&config, 0, sizeof(config));
    config.channels = DAC_CHANNELS;
    config.rate = DAC_RATE;
    config.period_size = BUFFER_FRAMES;
    config.period_count = 4;
    config.format = PCM_FORMAT_S16_LE;

    struct pcm *pcm = pcm_open(0, 0, PCM_OUT, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        printf("[DAP] Failed to open PCM DAC card 0 device 0: %s\n", pcm ? pcm_get_error(pcm) : "null");
        if (pcm) pcm_close(pcm);
        pcm = NULL;
    }

    static int16_t in_buffer[BUFFER_FRAMES * 8];
    static int16_t dac_buffer[BUFFER_FRAMES * DAC_CHANNELS];

    while (1) {
        pthread_mutex_lock(&ctx.lock);
        while (ctx.state != STATE_PLAYING || ctx.current_index < 0 || ctx.current_index >= ctx.count) {
            pthread_cond_wait(&ctx.cond, &ctx.lock);
        }

        char track_path[MAX_PATH];
        strncpy(track_path, ctx.tracks[ctx.current_index].path, MAX_PATH);
        ctx.tape_stop_requested = 0;
        ctx.spin_up_active = 1;
        ctx.tape_speed = 0.1;
        pthread_mutex_unlock(&ctx.lock);

        printf("[DAP] Opening track: %s\n", track_path);
        const char *ext = strrchr(track_path, '.');

        drflac *flac = NULL;
        drwav wav;
        int is_wav = 0;
        mp3dec_ex_t mp3;
        int is_mp3 = 0;

        uint32_t srate = 44100;
        uint32_t ch = 2;
        uint64_t total_f = 0;

        if (ext && strcasecmp(ext, ".flac") == 0) {
            flac = drflac_open_file(track_path, NULL);
            if (!flac) {
                printf("[DAP] Error opening FLAC: %s\n", track_path);
                pthread_mutex_lock(&ctx.lock);
                ctx.current_index = get_next_index();
                pthread_mutex_unlock(&ctx.lock);
                continue;
            }
            srate = flac->sampleRate;
            ch = flac->channels;
            total_f = flac->totalPCMFrameCount;
        } else if (ext && strcasecmp(ext, ".wav") == 0) {
            if (!drwav_init_file(&wav, track_path, NULL)) {
                printf("[DAP] Error opening WAV: %s\n", track_path);
                pthread_mutex_lock(&ctx.lock);
                ctx.current_index = get_next_index();
                pthread_mutex_unlock(&ctx.lock);
                continue;
            }
            is_wav = 1;
            srate = wav.sampleRate;
            ch = wav.channels;
            total_f = wav.totalPCMFrameCount;
        } else if (ext && strcasecmp(ext, ".mp3") == 0) {
            if (mp3dec_ex_open(&mp3, track_path, MP3D_SEEK_TO_SAMPLE)) {
                printf("[DAP] Error opening MP3: %s\n", track_path);
                pthread_mutex_lock(&ctx.lock);
                ctx.current_index = get_next_index();
                pthread_mutex_unlock(&ctx.lock);
                continue;
            }
            is_mp3 = 1;
            srate = mp3.info.hz;
            ch = mp3.info.channels;
            total_f = mp3.samples / ch;
        } else {
            continue;
        }

        pthread_mutex_lock(&ctx.lock);
        ctx.sample_rate = srate;
        ctx.channels = ch;
        ctx.total_frames = total_f;
        ctx.current_pos_frames = 0;
        pthread_mutex_unlock(&ctx.lock);

        if (!pcm) {
            pcm = pcm_open(0, 0, PCM_OUT, &config);
        }

        int playing = 1;
        while (playing) {
            pthread_mutex_lock(&ctx.lock);
            if (ctx.state == STATE_PAUSED) {
                pthread_mutex_unlock(&ctx.lock);
                usleep(50000);
                continue;
            }
            if (ctx.state == STATE_STOPPED) {
                playing = 0;
                pthread_mutex_unlock(&ctx.lock);
                break;
            }

            int tape_stop = ctx.tape_stop_requested;
            int spin_up = ctx.spin_up_active;
            double speed = ctx.tape_speed;
            int vol = ctx.volume;
            pthread_mutex_unlock(&ctx.lock);

            // Spin-up logic (0.1 -> 1.0)
            if (spin_up) {
                speed += 0.08;
                if (speed >= 1.0) {
                    speed = 1.0;
                    pthread_mutex_lock(&ctx.lock);
                    ctx.spin_up_active = 0;
                    ctx.tape_speed = 1.0;
                    pthread_mutex_unlock(&ctx.lock);
                } else {
                    pthread_mutex_lock(&ctx.lock);
                    ctx.tape_speed = speed;
                    pthread_mutex_unlock(&ctx.lock);
                }
            }

            // Tape-stop logic (1.0 -> 0.0)
            if (tape_stop) {
                speed -= 0.06;
                if (speed <= 0.02) {
                    speed = 0.0;
                    pthread_mutex_lock(&ctx.lock);
                    ctx.tape_stop_requested = 0;
                    ctx.tape_speed = 0.0;
                    ctx.current_index = get_next_index();
                    pthread_mutex_unlock(&ctx.lock);
                    playing = 0;
                    break;
                } else {
                    pthread_mutex_lock(&ctx.lock);
                    ctx.tape_speed = speed;
                    pthread_mutex_unlock(&ctx.lock);
                }
            }

            // Calculate input frames needed for 1024 output frames
            double ratio = (double)srate / (double)DAC_RATE;
            int in_needed = (int)(BUFFER_FRAMES * ratio) + 2;
            if (in_needed > (BUFFER_FRAMES * 4)) in_needed = BUFFER_FRAMES * 4;

            uint64_t frames_read = 0;
            if (flac) {
                frames_read = drflac_read_pcm_frames_s16(flac, in_needed, in_buffer);
            } else if (is_wav) {
                frames_read = drwav_read_pcm_frames_s16(&wav, in_needed, in_buffer);
            } else if (is_mp3) {
                size_t samples_read = mp3dec_ex_read(&mp3, in_buffer, in_needed * ch);
                frames_read = samples_read / ch;
            }

            if (frames_read == 0) {
                // Auto-advance
                pthread_mutex_lock(&ctx.lock);
                ctx.current_index = get_next_index();
                pthread_mutex_unlock(&ctx.lock);
                playing = 0;
                break;
            }

            // Resample / format to 48000 Hz Stereo
            for (int j = 0; j < BUFFER_FRAMES; j++) {
                double src_pos = (double)j * ratio;
                int idx0 = (int)src_pos;
                int idx1 = idx0 + 1;
                if (idx0 >= (int)frames_read) idx0 = (int)frames_read - 1;
                if (idx1 >= (int)frames_read) idx1 = idx0;
                double frac = src_pos - (double)idx0;

                if (ch == 1) {
                    int16_t s0 = in_buffer[idx0];
                    int16_t s1 = in_buffer[idx1];
                    int16_t val = (int16_t)(s0 + (s1 - s0) * frac);
                    dac_buffer[j * 2 + 0] = val;
                    dac_buffer[j * 2 + 1] = val;
                } else {
                    int16_t l0 = in_buffer[idx0 * ch + 0];
                    int16_t l1 = in_buffer[idx1 * ch + 0];
                    int16_t r0 = in_buffer[idx0 * ch + 1];
                    int16_t r1 = in_buffer[idx1 * ch + 1];
                    dac_buffer[j * 2 + 0] = (int16_t)(l0 + (l1 - l0) * frac);
                    dac_buffer[j * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);
                }
            }

            // Apply Tape-stop & Volume
            apply_tape_stop_and_volume(dac_buffer, BUFFER_FRAMES, DAC_CHANNELS, speed, vol);

            // Write to ALSA DAC (with hotplug / dynamic reconnection)
            if (!pcm) {
                pcm = pcm_open(0, 0, PCM_OUT, &config);
                if (pcm && !pcm_is_ready(pcm)) {
                    pcm_close(pcm);
                    pcm = NULL;
                }
            }
            if (pcm) {
                int err = pcm_writei(pcm, dac_buffer, BUFFER_FRAMES);
                if (err < 0) {
                    pcm_close(pcm);
                    pcm = NULL;
                }
            } else {
                usleep(21000);
            }

            pthread_mutex_lock(&ctx.lock);
            ctx.current_pos_frames += frames_read;
            pthread_mutex_unlock(&ctx.lock);
        }

        if (flac) drflac_close(flac);
        if (is_wav) drwav_uninit(&wav);
        if (is_mp3) mp3dec_ex_close(&mp3);
    }

    if (pcm) pcm_close(pcm);
    return NULL;
}

void *tcp_server_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    int bound = 0;
    for (int retry = 0; retry < 15; retry++) {
        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            bound = 1;
            break;
        }
        usleep(500000);
    }
    if (!bound) {
        close(server_fd);
        return NULL;
    }

    listen(server_fd, 5);
    printf("[DAP] Remote server listening on 0.0.0.0:%d\n", PORT);

    char buf[1024];
    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ssize_t n = read(client, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
                buf[--n] = '\0';
            }

            char resp[2048] = "OK\n";

            if (strncmp(buf, "PLAY", 4) == 0) {
                pthread_mutex_lock(&ctx.lock);
                ctx.state = STATE_PLAYING;
                ctx.spin_up_active = 1;
                ctx.tape_speed = 0.1;
                pthread_cond_signal(&ctx.cond);
                pthread_mutex_unlock(&ctx.lock);
                snprintf(resp, sizeof(resp), "{\"status\": \"playing\"}\n");
            } else if (strncmp(buf, "PAUSE", 5) == 0) {
                pthread_mutex_lock(&ctx.lock);
                if (ctx.state == STATE_PLAYING) {
                    ctx.state = STATE_PAUSED;
                } else if (ctx.state == STATE_PAUSED) {
                    ctx.state = STATE_PLAYING;
                    ctx.spin_up_active = 1;
                    ctx.tape_speed = 0.1;
                }
                pthread_cond_signal(&ctx.cond);
                pthread_mutex_unlock(&ctx.lock);
                snprintf(resp, sizeof(resp), "{\"status\": \"%s\"}\n", ctx.state == STATE_PLAYING ? "playing" : "paused");
            } else if (strncmp(buf, "NEXT", 4) == 0) {
                pthread_mutex_lock(&ctx.lock);
                ctx.tape_stop_requested = 1;
                pthread_mutex_unlock(&ctx.lock);
                snprintf(resp, sizeof(resp), "{\"status\": \"next_tape_stop\"}\n");
            } else if (strncmp(buf, "PREV", 4) == 0) {
                pthread_mutex_lock(&ctx.lock);
                ctx.tape_stop_requested = 1;
                ctx.current_index = get_prev_index();
                pthread_mutex_unlock(&ctx.lock);
                snprintf(resp, sizeof(resp), "{\"status\": \"prev_tape_stop\"}\n");
            } else if (strncmp(buf, "SHUFFLE", 7) == 0) {
                pthread_mutex_lock(&ctx.lock);
                ctx.shuffle = !ctx.shuffle;
                snprintf(resp, sizeof(resp), "{\"shuffle\": %d}\n", ctx.shuffle);
                pthread_mutex_unlock(&ctx.lock);
            } else if (strncmp(buf, "VOL ", 4) == 0) {
                int v = atoi(buf + 4);
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                pthread_mutex_lock(&ctx.lock);
                ctx.volume = v;
                pthread_mutex_unlock(&ctx.lock);
                snprintf(resp, sizeof(resp), "{\"volume\": %d}\n", v);
            } else if (strncmp(buf, "SCAN", 4) == 0) {
                init_hardware_and_storage();
                playlist_clear();
                scan_directory("/mnt/exUDISK");
                scan_directory("/mnt/UDISK");
                pthread_mutex_lock(&ctx.lock);
                if (ctx.count > 0 && ctx.current_index < 0) {
                    ctx.current_index = 0;
                    ctx.state = STATE_PLAYING;
                    ctx.spin_up_active = 1;
                    ctx.tape_speed = 0.1;
                    pthread_cond_signal(&ctx.cond);
                }
                snprintf(resp, sizeof(resp), "{\"total_tracks\": %d}\n", ctx.count);
                pthread_mutex_unlock(&ctx.lock);
            } else if (strncmp(buf, "EXEC ", 5) == 0) {
                const char *cmd = buf + 5;
                char exec_buf[16384];
                char full_cmd[1024];
                snprintf(full_cmd, sizeof(full_cmd), "( %s ) 2>&1", cmd);
                FILE *fp = popen(full_cmd, "r");
                if (fp) {
                    size_t bytes = fread(exec_buf, 1, sizeof(exec_buf) - 1, fp);
                    exec_buf[bytes] = '\0';
                    pclose(fp);
                    write(client, exec_buf, bytes);
                    close(client);
                    continue;
                } else {
                    snprintf(resp, sizeof(resp), "ERROR: popen failed\n");
                }
            } else if (strncmp(buf, "STATUS", 6) == 0) {
                pthread_mutex_lock(&ctx.lock);
                const char *st = (ctx.state == STATE_PLAYING) ? "playing" : (ctx.state == STATE_PAUSED) ? "paused" : "stopped";
                const char *fn = (ctx.current_index >= 0 && ctx.current_index < ctx.count) ? ctx.tracks[ctx.current_index].filename : "";
                double pos_sec = ctx.sample_rate ? (double)ctx.current_pos_frames / ctx.sample_rate : 0.0;
                double dur_sec = ctx.sample_rate ? (double)ctx.total_frames / ctx.sample_rate : 0.0;

                snprintf(resp, sizeof(resp),
                    "{\"state\": \"%s\", \"track\": \"%s\", \"index\": %d, \"total\": %d, \"pos\": %.1f, \"dur\": %.1f, \"vol\": %d, \"shuffle\": %d}\n",
                    st, fn, ctx.current_index + 1, ctx.count, pos_sec, dur_sec, ctx.volume, ctx.shuffle);
                pthread_mutex_unlock(&ctx.lock);
            }

            write(client, resp, strlen(resp));
        }
        close(client);
    }
    close(server_fd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    printf("=== Allwinner V821 Autonomous Hi-Fi DAP Engine ===\n");
    memset(&ctx, 0, sizeof(ctx));
    ctx.current_index = -1;
    ctx.volume = 30; // Default 30% volume as requested
    ctx.tape_speed = 1.0;
    pthread_mutex_init(&ctx.lock, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024); // 1 MB stack
    pthread_t th_audio, th_tcp;
    pthread_create(&th_tcp, &attr, tcp_server_thread, NULL);
    pthread_create(&th_audio, &attr, audio_playback_thread, NULL);
    pthread_attr_destroy(&attr);

    // Initialize hardware drivers & storage
    init_hardware_and_storage();

    // Initial scan of internal and external storage
    scan_directory("/mnt/exUDISK");
    scan_directory("/mnt/UDISK");

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            playlist_add(argv[i]);
        }
    }

    pthread_mutex_lock(&ctx.lock);
    if (ctx.count > 0) {
        ctx.current_index = 0;
        ctx.state = STATE_PLAYING;
        ctx.spin_up_active = 1;
        ctx.tape_speed = 0.1;
        pthread_cond_signal(&ctx.cond);
        printf("[DAP] Found %d tracks in storage. Autoplaying track 1.\n", ctx.count);
    }
    pthread_mutex_unlock(&ctx.lock);

    pthread_join(th_audio, NULL);
    pthread_join(th_tcp, NULL);
    return 0;
}
