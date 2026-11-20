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
#define FIFO_MAX_FRAMES 8192
#define MAX_TRACKS 1024
#define MAX_PATH 512

typedef enum {
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} PlaybackState;

typedef enum {
    ACTION_NONE,
    ACTION_NEXT,
    ACTION_PREV,
    ACTION_PAUSE,
    ACTION_INDEX
} TapeAction;

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
    TapeAction tape_action;
    int target_index;
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

static int track_cmp(const void *a, const void *b) {
    const Track *ta = (const Track *)a;
    const Track *tb = (const Track *)b;
    return strcasecmp(ta->filename, tb->filename);
}

void playlist_sort() {
    pthread_mutex_lock(&ctx.lock);
    if (ctx.count > 1) {
        qsort(ctx.tracks, ctx.count, sizeof(Track), track_cmp);
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

    // 1. If Music is already accessible, load audio drivers and return
    if (access("/mnt/exUDISK/Music", F_OK) == 0) {
        system("insmod /mnt/exUDISK/modules/soundcore.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-timer.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-pcm.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-hwdep.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-rawmidi.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-usbmidi-lib.ko 2>/dev/null");
        system("insmod /mnt/exUDISK/modules/snd-usb-audio.ko 2>/dev/null");
        return;
    }

    // 2. Load SCSI storage drivers from flash
    system("insmod /mnt/UDISK/modules/v821_compat.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/scsi_mod.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/sd_mod.ko 2>/dev/null");
    system("insmod /mnt/UDISK/modules/usb-storage.ko 2>/dev/null");

    // 3. Mount USB storage using active /proc/partitions
    system("mkdir -p /mnt/exUDISK");
    for (int i = 0; i < 8; i++) {
        if (access("/mnt/exUDISK/Music", F_OK) == 0) break;
        FILE *fp = popen("grep sd /proc/partitions 2>/dev/null | awk '{print $4}' | grep '[0-9]' | head -n 1", "r");
        if (fp) {
            char part[32] = {0};
            if (fgets(part, sizeof(part), fp)) {
                char *nl = strchr(part, '\n'); if (nl) *nl = '\0';
                char *cr = strchr(part, '\r'); if (cr) *cr = '\0';
                if (strlen(part) > 0) {
                    char mcmd[128];
                    snprintf(mcmd, sizeof(mcmd), "mount -t vfat -o ro,utf8 /dev/%s /mnt/exUDISK 2>/dev/null || mount -o ro /dev/%s /mnt/exUDISK 2>/dev/null", part, part);
                    system(mcmd);
                }
            }
            pclose(fp);
        }
        if (access("/mnt/exUDISK/Music", F_OK) == 0) break;
        usleep(500000);
    }

    // 4. Load Audio drivers from USB storage
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

    static int16_t fifo_buf[FIFO_MAX_FRAMES * DAC_CHANNELS];
    static int16_t mono_temp[FIFO_MAX_FRAMES];
    static int16_t dac_buffer[BUFFER_FRAMES * DAC_CHANNELS];

    while (1) {
        pthread_mutex_lock(&ctx.lock);
        while (ctx.state != STATE_PLAYING || ctx.current_index < 0 || ctx.current_index >= ctx.count) {
            pthread_cond_wait(&ctx.cond, &ctx.lock);
        }

        char track_path[MAX_PATH];
        strncpy(track_path, ctx.tracks[ctx.current_index].path, MAX_PATH);
        ctx.tape_stop_requested = 0;
        ctx.tape_action = ACTION_NONE;
        ctx.spin_up_active = 1;
        ctx.tape_speed = 0.15;
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

        int fifo_count = 0;
        double frac_pos = 0.0;
        int playing = 1;

        while (playing) {
            pthread_mutex_lock(&ctx.lock);
            if (ctx.state == STATE_PAUSED && !ctx.spin_up_active && !ctx.tape_stop_requested) {
                pthread_mutex_unlock(&ctx.lock);
                usleep(30000);
                continue;
            }
            if (ctx.state == STATE_STOPPED) {
                playing = 0;
                pthread_mutex_unlock(&ctx.lock);
                break;
            }

            int tape_stop = ctx.tape_stop_requested;
            TapeAction action = ctx.tape_action;
            int target_idx = ctx.target_index;
            int spin_up = ctx.spin_up_active;
            double speed = ctx.tape_speed;
            int vol = ctx.volume;
            pthread_mutex_unlock(&ctx.lock);

            // Spin-up logic (0.15 -> 1.0)
            if (spin_up) {
                speed += 0.05;
                if (speed >= 1.0) {
                    speed = 1.0;
                    spin_up = 0;
                }
                pthread_mutex_lock(&ctx.lock);
                ctx.tape_speed = speed;
                ctx.spin_up_active = spin_up;
                pthread_mutex_unlock(&ctx.lock);
            }
            // Tape-stop logic (1.0 -> 0.0)
            else if (tape_stop) {
                speed -= 0.05;
                if (speed <= 0.05) {
                    speed = 0.0;
                    pthread_mutex_lock(&ctx.lock);
                    ctx.tape_stop_requested = 0;
                    ctx.tape_speed = 0.0;
                    if (action == ACTION_PAUSE) {
                        ctx.state = STATE_PAUSED;
                        ctx.tape_action = ACTION_NONE;
                        pthread_mutex_unlock(&ctx.lock);
                        continue;
                    } else if (action == ACTION_NEXT) {
                        ctx.current_index = get_next_index();
                        ctx.tape_action = ACTION_NONE;
                        playing = 0;
                        pthread_mutex_unlock(&ctx.lock);
                        break;
                    } else if (action == ACTION_PREV) {
                        ctx.current_index = get_prev_index();
                        ctx.tape_action = ACTION_NONE;
                        playing = 0;
                        pthread_mutex_unlock(&ctx.lock);
                        break;
                    } else if (action == ACTION_INDEX) {
                        ctx.current_index = target_idx;
                        ctx.tape_action = ACTION_NONE;
                        playing = 0;
                        pthread_mutex_unlock(&ctx.lock);
                        break;
                    } else {
                        ctx.current_index = get_next_index();
                        playing = 0;
                        pthread_mutex_unlock(&ctx.lock);
                        break;
                    }
                } else {
                    pthread_mutex_lock(&ctx.lock);
                    ctx.tape_speed = speed;
                    pthread_mutex_unlock(&ctx.lock);
                }
            }

            // Continuous Fractional Stream Resampler
            // Playback speed modulates the step through the source audio:
            // Slower speed = smaller step = lower pitch and slower sound!
            double effective_ratio = ((double)srate / (double)DAC_RATE) * speed;
            if (effective_ratio < 0.02) effective_ratio = 0.02;

            // Frames needed in fifo to produce BUFFER_FRAMES output frames
            int needed_in_fifo = (int)(frac_pos + (double)BUFFER_FRAMES * effective_ratio) + 4;
            if (needed_in_fifo >= fifo_count) {
                // Shift consumed frames
                int consumed = (int)frac_pos;
                if (consumed > 0) {
                    int remaining = fifo_count - consumed;
                    if (remaining > 0) {
                        memmove(fifo_buf, fifo_buf + consumed * DAC_CHANNELS, remaining * DAC_CHANNELS * sizeof(int16_t));
                    }
                    fifo_count = remaining;
                    frac_pos -= (double)consumed;
                    pthread_mutex_lock(&ctx.lock);
                    ctx.current_pos_frames += consumed;
                    pthread_mutex_unlock(&ctx.lock);
                }

                // Read more frames into fifo
                int to_read = FIFO_MAX_FRAMES - fifo_count;
                if (to_read > 0) {
                    int actually_read = 0;
                    if (flac) {
                        if (ch == 1) {
                            actually_read = (int)drflac_read_pcm_frames_s16(flac, to_read, mono_temp);
                            for (int m = 0; m < actually_read; m++) {
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 0] = mono_temp[m];
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 1] = mono_temp[m];
                            }
                        } else {
                            actually_read = (int)drflac_read_pcm_frames_s16(flac, to_read, fifo_buf + fifo_count * DAC_CHANNELS);
                        }
                    } else if (is_wav) {
                        if (ch == 1) {
                            actually_read = (int)drwav_read_pcm_frames_s16(&wav, to_read, mono_temp);
                            for (int m = 0; m < actually_read; m++) {
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 0] = mono_temp[m];
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 1] = mono_temp[m];
                            }
                        } else {
                            actually_read = (int)drwav_read_pcm_frames_s16(&wav, to_read, fifo_buf + fifo_count * DAC_CHANNELS);
                        }
                    } else if (is_mp3) {
                        if (ch == 1) {
                            actually_read = (int)mp3dec_ex_read(&mp3, mono_temp, to_read);
                            for (int m = 0; m < actually_read; m++) {
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 0] = mono_temp[m];
                                fifo_buf[(fifo_count + m) * DAC_CHANNELS + 1] = mono_temp[m];
                            }
                        } else {
                            actually_read = (int)(mp3dec_ex_read(&mp3, fifo_buf + fifo_count * DAC_CHANNELS, to_read * ch) / ch);
                        }
                    }
                    fifo_count += actually_read;
                    if (actually_read == 0 && fifo_count <= 2) {
                        // EOF reached
                        pthread_mutex_lock(&ctx.lock);
                        ctx.current_index = get_next_index();
                        pthread_mutex_unlock(&ctx.lock);
                        playing = 0;
                        break;
                    }
                }
            }

            // Generate output buffer with linear interpolation & tape volume taper
            float vol_factor = ((float)vol / 100.0f) * (float)(speed * speed);
            for (int j = 0; j < BUFFER_FRAMES; j++) {
                int idx0 = (int)frac_pos;
                int idx1 = idx0 + 1;
                if (idx1 >= fifo_count) {
                    idx1 = (fifo_count > 0) ? fifo_count - 1 : 0;
                    if (idx0 >= fifo_count) idx0 = idx1;
                }
                double frac = frac_pos - (double)idx0;
                int16_t l0 = fifo_buf[idx0 * DAC_CHANNELS + 0];
                int16_t l1 = fifo_buf[idx1 * DAC_CHANNELS + 0];
                int16_t r0 = fifo_buf[idx0 * DAC_CHANNELS + 1];
                int16_t r1 = fifo_buf[idx1 * DAC_CHANNELS + 1];

                float l = ((float)l0 + (float)(l1 - l0) * (float)frac) * vol_factor;
                float r = ((float)r0 + (float)(r1 - r0) * (float)frac) * vol_factor;
                if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
                if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
                dac_buffer[j * DAC_CHANNELS + 0] = (int16_t)l;
                dac_buffer[j * DAC_CHANNELS + 1] = (int16_t)r;

                frac_pos += effective_ratio;
            }

            // Write to ALSA DAC
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

        int n = read(client, buf, sizeof(buf) - 1);
        if (n <= 0) { close(client); continue; }
        buf[n] = '\0';

        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
            buf[--n] = '\0';
        }

        char resp[2048] = "OK\n";

        if (strncmp(buf, "PLAY_INDEX ", 11) == 0) {
            int target = atoi(buf + 11) - 1;
            pthread_mutex_lock(&ctx.lock);
            if (target >= 0 && target < ctx.count) {
                ctx.target_index = target;
                ctx.tape_action = ACTION_INDEX;
                ctx.tape_stop_requested = 1;
                pthread_cond_signal(&ctx.cond);
            }
            pthread_mutex_unlock(&ctx.lock);
            snprintf(resp, sizeof(resp), "{\"status\": \"index_tape_stop\", \"target\": %d}\n", target + 1);
        } else if (strncmp(buf, "PLAY", 4) == 0) {
            pthread_mutex_lock(&ctx.lock);
            if (ctx.state == STATE_PAUSED) {
                ctx.state = STATE_PLAYING;
                ctx.spin_up_active = 1;
                ctx.tape_speed = 0.15;
            } else if (ctx.state == STATE_STOPPED) {
                ctx.state = STATE_PLAYING;
                ctx.spin_up_active = 1;
                ctx.tape_speed = 0.15;
                if (ctx.current_index < 0 && ctx.count > 0) ctx.current_index = 0;
            }
            pthread_cond_signal(&ctx.cond);
            pthread_mutex_unlock(&ctx.lock);
            snprintf(resp, sizeof(resp), "{\"status\": \"playing\"}\n");
        } else if (strncmp(buf, "PAUSE", 5) == 0) {
            pthread_mutex_lock(&ctx.lock);
            if (ctx.state == STATE_PLAYING) {
                ctx.tape_action = ACTION_PAUSE;
                ctx.tape_stop_requested = 1;
            } else if (ctx.state == STATE_PAUSED) {
                ctx.state = STATE_PLAYING;
                ctx.spin_up_active = 1;
                ctx.tape_speed = 0.15;
                pthread_cond_signal(&ctx.cond);
            }
            pthread_mutex_unlock(&ctx.lock);
            snprintf(resp, sizeof(resp), "{\"status\": \"pause_tape_stop\"}\n");
        } else if (strncmp(buf, "NEXT", 4) == 0) {
            pthread_mutex_lock(&ctx.lock);
            ctx.tape_action = ACTION_NEXT;
            ctx.tape_stop_requested = 1;
            pthread_cond_signal(&ctx.cond);
            pthread_mutex_unlock(&ctx.lock);
            snprintf(resp, sizeof(resp), "{\"status\": \"next_tape_stop\"}\n");
        } else if (strncmp(buf, "PREV", 4) == 0) {
            pthread_mutex_lock(&ctx.lock);
            ctx.tape_action = ACTION_PREV;
            ctx.tape_stop_requested = 1;
            pthread_cond_signal(&ctx.cond);
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
            playlist_clear();
            scan_directory("/mnt/exUDISK");
            scan_directory("/mnt/UDISK");
            playlist_sort();
            pthread_mutex_lock(&ctx.lock);
            if (ctx.count > 0 && ctx.current_index < 0) {
                ctx.current_index = 0;
                ctx.state = STATE_PLAYING;
                ctx.spin_up_active = 1;
                ctx.tape_speed = 0.15;
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
    playlist_sort();

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            playlist_add(argv[i]);
        }
        playlist_sort();
    }

    pthread_mutex_lock(&ctx.lock);
    if (ctx.count > 0) {
        ctx.current_index = 0;
        ctx.state = STATE_PLAYING;
        ctx.spin_up_active = 1;
        ctx.tape_speed = 0.15;
        pthread_cond_signal(&ctx.cond);
        printf("[DAP] Found %d tracks in storage. Autoplaying track 1.\n", ctx.count);
    }
    pthread_mutex_unlock(&ctx.lock);

    pthread_join(th_audio, NULL);
    pthread_join(th_tcp, NULL);
    return 0;
}
