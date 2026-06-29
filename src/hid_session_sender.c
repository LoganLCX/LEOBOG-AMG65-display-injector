#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define TARGET_VID 0x0C45
#define TARGET_PID 0x800A
#define TARGET_MI "MI_02"
#define REPORT_LEN 65
#define DEFAULT_DELAY_MS 10
#define DEFAULT_SESSION_GAP_MS 0
#define DEFAULT_MARKOV_FRAME_DELAY_MS 0
#define SCREEN_W 63
#define SCREEN_H 5
#define PIXELS_PER_FRAME (SCREEN_W * SCREEN_H)
#define SPRITE_W 27
#define MAX_OFFSET (SCREEN_W - SPRITE_W)

#define IDLE_DESCEND_P 0.001
#define IDLE_ANIM_P 0.03
#define LOOK_CHAIN_P 0.20
#define BLINK_VARIANT_P 0.20
#define BLINK_VARIANT_ADVANCE_P 0.20
#define IDLE_MIN_HOLD_FRAMES 12
#define ASCEND_INVERT_P 0.05

typedef uint32_t Color;

typedef struct {
    unsigned char data[REPORT_LEN];
} Packet65;

typedef struct {
    size_t frame_count;
    Color *pixels;
} Asset;

typedef enum {
    STATE_HIDDEN = 0,
    STATE_ASCEND,
    STATE_DESCEND,
    STATE_IDLE1,
    STATE_IDLE2,
    STATE_BLINK,
    STATE_BLINK_VARIANT,
    STATE_LOOK1,
    STATE_LOOK2,
    STATE_LOOK3
} RuntimeState;

typedef struct {
    Asset bg;
    Asset idle1;
    Asset idle2;
    Asset blink;
    Asset descend;
    Asset ascend;
    Asset look1;
    Asset look2;
    Asset look3;
    RuntimeState state;
    size_t index;
    int offset;
    int state_hold;
    bool invert_cycle;
} MarkovPlayer;

static volatile LONG g_stop_requested = 0;

static void get_exe_dir(char *out, size_t out_len) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_len);
    if (n == 0 || n >= out_len) {
        strncpy(out, ".", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    for (DWORD i = n; i > 0; --i) {
        if (out[i] == '\\' || out[i] == '/') {
            out[i] = '\0';
            return;
        }
    }
    strncpy(out, ".", out_len - 1);
    out[out_len - 1] = '\0';
}

static void join_path2(char *out, size_t out_len, const char *a, const char *b) {
    snprintf(out, out_len, "%s\\%s", a, b);
}

static bool ascii_icontains(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen) {
            char a = p[i];
            char b = needle[i];
            if (!a) return false;
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool is_hex6(const char *p) {
    for (int i = 0; i < 6; ++i) {
        char c = p[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

static unsigned hex2(char a, char b) {
    unsigned x = 0;
    sscanf((char[3]){a, b, '\0'}, "%x", &x);
    return x;
}

static Color make_color(unsigned r, unsigned g, unsigned b) {
    return ((Color)r << 16) | ((Color)g << 8) | (Color)b;
}

static unsigned color_r(Color c) { return (c >> 16) & 0xFF; }
static unsigned color_g(Color c) { return (c >> 8) & 0xFF; }
static unsigned color_b(Color c) { return c & 0xFF; }

static Color remap_color(Color c) {
    unsigned r0 = color_r(c);
    unsigned g0 = color_g(c);
    unsigned b0 = color_b(c);
    if (r0 == 0 && g0 == 0 && b0 == 0) return 0;

    double luma = 0.299 * r0 + 0.587 * g0 + 0.114 * b0;
    if (luma >= 190.0) {
        unsigned maxv = r0;
        if (g0 > maxv) maxv = g0;
        if (b0 > maxv) maxv = b0;
        unsigned minv = r0;
        if (g0 < minv) minv = g0;
        if (b0 < minv) minv = b0;
        unsigned spread = maxv - minv;

        if (spread < 16) {
            double v = (maxv / 255.0);
            v += 0.04;
            if (v > 1.0) v = 1.0;
            unsigned out = (unsigned)lround(v * 255.0);
            return make_color(out, out, out);
        } else {
            double luma2 = luma * 0.96 + 0.005 * 255.0;
            double chroma_scale = 1.45;
            int r = (int)lround(luma2 + (r0 - luma) * chroma_scale);
            int g = (int)lround(luma2 + (g0 - luma) * chroma_scale);
            int b = (int)lround(luma2 + (b0 - luma) * chroma_scale);
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            return make_color((unsigned)r, (unsigned)g, (unsigned)b);
        }
    }

    if (luma <= 8.0) return 0;
    double low = 18.0;
    double high = 236.0;
    double x = (luma - low) / (high - low);
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    x = pow(x, 0.92);
    x = (x - 0.5) * 1.12 + 0.5;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    double target_luma = 255.0 * x;
    double scale = target_luma / luma;
    int r = (int)lround(r0 * scale);
    int g = (int)lround(g0 * scale);
    int b = (int)lround(b0 * scale);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return make_color((unsigned)r, (unsigned)g, (unsigned)b);
}

static bool load_asset_json_colors(const char *path, Asset *asset) {
    memset(asset, 0, sizeof(*asset));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "failed to open asset: %s\n", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }
    fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[size] = '\0';

    size_t color_count = 0;
    for (long i = 0; i + 8 < size; ++i) {
        if (buf[i] == '"' && buf[i + 1] == '#' && is_hex6(&buf[i + 2]) && buf[i + 8] == '"') {
            ++color_count;
            i += 8;
        }
    }
    if (color_count == 0 || (color_count % PIXELS_PER_FRAME) != 0) {
        fprintf(stderr, "unexpected color count in %s: %zu\n", path, color_count);
        free(buf);
        return false;
    }

    asset->frame_count = color_count / PIXELS_PER_FRAME;
    asset->pixels = (Color *)malloc(color_count * sizeof(Color));
    if (!asset->pixels) {
        free(buf);
        return false;
    }

    size_t idx = 0;
    for (long i = 0; i + 8 < size && idx < color_count; ++i) {
        if (buf[i] == '"' && buf[i + 1] == '#' && is_hex6(&buf[i + 2]) && buf[i + 8] == '"') {
            unsigned r = hex2(buf[i + 2], buf[i + 3]);
            unsigned g = hex2(buf[i + 4], buf[i + 5]);
            unsigned b = hex2(buf[i + 6], buf[i + 7]);
            asset->pixels[idx++] = make_color(r, g, b);
            i += 8;
        }
    }
    free(buf);
    return idx == color_count;
}

static void free_asset(Asset *asset) {
    free(asset->pixels);
    asset->pixels = NULL;
    asset->frame_count = 0;
}

static bool load_asset_by_name(const char *dir, const char *name, Asset *asset) {
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    return load_asset_json_colors(path, asset);
}

static const Color *asset_frame(const Asset *asset, size_t idx) {
    return asset->pixels + (idx * PIXELS_PER_FRAME);
}

static bool clone_reversed_asset(const Asset *src, Asset *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->frame_count = src->frame_count;
    size_t total = src->frame_count * PIXELS_PER_FRAME;
    dst->pixels = (Color *)malloc(total * sizeof(Color));
    if (!dst->pixels) return false;
    for (size_t i = 0; i < src->frame_count; ++i) {
        memcpy(
            dst->pixels + i * PIXELS_PER_FRAME,
            src->pixels + (src->frame_count - 1 - i) * PIXELS_PER_FRAME,
            PIXELS_PER_FRAME * sizeof(Color)
        );
    }
    return true;
}

static void overlay_frame(const Color *bg, const Color *fg, int offset, Color *out) {
    memcpy(out, bg, PIXELS_PER_FRAME * sizeof(Color));
    for (int i = 0; i < PIXELS_PER_FRAME; ++i) {
        Color c = fg[i];
        if (c == 0) continue;
        int row = i / SCREEN_W;
        int col = i % SCREEN_W;
        int target_col = col + offset;
        if (target_col >= 0 && target_col < SCREEN_W) {
            out[row * SCREEN_W + target_col] = c;
        }
    }
    for (int i = 0; i < PIXELS_PER_FRAME; ++i) {
        out[i] = remap_color(out[i]);
    }
}

static void flip_vertical(const Color *src, Color *dst) {
    for (int y = 0; y < SCREEN_H; ++y) {
        memcpy(
            dst + y * SCREEN_W,
            src + (SCREEN_H - 1 - y) * SCREEN_W,
            SCREEN_W * sizeof(Color)
        );
    }
}

static void build_stream_from_frame(const Color *frame, unsigned char *stream_out) {
    const int block_starts[5] = {0, 14, 28, 42, 56};
    const int block_ends[5] = {14, 28, 42, 56, 63};
    size_t out_idx = 0;
    for (int bi = 0; bi < 5; ++bi) {
        for (int y = 0; y < SCREEN_H; ++y) {
            int row_base = y * SCREEN_W;
            for (int x = block_starts[bi]; x < block_ends[bi]; ++x) {
                Color c = frame[row_base + x];
                stream_out[out_idx++] = (unsigned char)color_r(c);
                stream_out[out_idx++] = (unsigned char)color_g(c);
                stream_out[out_idx++] = (unsigned char)color_b(c);
            }
        }
    }
}

static void build_session_packets_from_frame(const Color *frame, Packet65 *packets_out) {
    static const unsigned char start18[REPORT_LEN] = {
        0x00,0x04,0x18
    };
    static const unsigned char start35[REPORT_LEN] = {
        0x00,0x04,0x35,0x00,0x00,0x00,0x00,0x00,0x00,0x0F
    };
    static const unsigned char end02[REPORT_LEN] = {
        0x00,0x04,0x02
    };
    unsigned char stream[PIXELS_PER_FRAME * 3];
    memset(packets_out, 0, sizeof(Packet65) * 19);
    memcpy(packets_out[0].data, start18, sizeof(start18));
    memcpy(packets_out[1].data, start35, sizeof(start35));
    build_stream_from_frame(frame, stream);
    for (int i = 0; i < 15; ++i) {
        packets_out[2 + i].data[0] = 0x00;
        size_t offset = (size_t)i * 64;
        size_t remaining = sizeof(stream) - offset;
        size_t chunk = remaining >= 64 ? 64 : remaining;
        memcpy(&packets_out[2 + i].data[1], stream + offset, chunk);
    }
    packets_out[17].data[52] = 0xAA;
    packets_out[17].data[53] = 0x55;
    memcpy(packets_out[18].data, end02, sizeof(end02));
}

static double frand01(void) {
    return rand() / ((double)RAND_MAX + 1.0);
}

static RuntimeState choose_idle_state(void) {
    return frand01() < (2.0 / 3.0) ? STATE_IDLE1 : STATE_IDLE2;
}

static RuntimeState choose_anim_state(void) {
    double roll = frand01();
    if (roll < 0.7) {
        return frand01() < BLINK_VARIANT_P ? STATE_BLINK_VARIANT : STATE_BLINK;
    }
    if (roll < 0.8) return STATE_LOOK1;
    if (roll < 0.9) return STATE_LOOK2;
    return STATE_LOOK3;
}

static bool init_markov_player(MarkovPlayer *p, const char *asset_dir) {
    memset(p, 0, sizeof(*p));
    if (!load_asset_by_name(asset_dir, "background.json", &p->bg)) return false;
    if (!load_asset_by_name(asset_dir, "idle1.json", &p->idle1)) return false;
    if (!load_asset_by_name(asset_dir, "idle2.json", &p->idle2)) return false;
    if (!load_asset_by_name(asset_dir, "blink.json", &p->blink)) return false;
    if (!load_asset_by_name(asset_dir, "descend.json", &p->descend)) return false;
    if (!clone_reversed_asset(&p->descend, &p->ascend)) return false;
    if (!load_asset_by_name(asset_dir, "look_left.json", &p->look1)) return false;
    if (!load_asset_by_name(asset_dir, "look_middle.json", &p->look2)) return false;
    if (!load_asset_by_name(asset_dir, "look_right.json", &p->look3)) return false;
    p->state = STATE_HIDDEN;
    p->index = 0;
    p->offset = rand() % (MAX_OFFSET + 1);
    p->state_hold = 0;
    p->invert_cycle = false;
    return true;
}

static void free_markov_player(MarkovPlayer *p) {
    free_asset(&p->bg);
    free_asset(&p->idle1);
    free_asset(&p->idle2);
    free_asset(&p->blink);
    free_asset(&p->descend);
    free_asset(&p->ascend);
    free_asset(&p->look1);
    free_asset(&p->look2);
    free_asset(&p->look3);
}

static void render_markov_next_frame(MarkovPlayer *p, Color *out) {
    Color tmp[PIXELS_PER_FRAME];
    for (;;) {
        switch (p->state) {
            case STATE_HIDDEN:
                memcpy(out, asset_frame(&p->bg, 0), PIXELS_PER_FRAME * sizeof(Color));
                for (int i = 0; i < PIXELS_PER_FRAME; ++i) out[i] = remap_color(out[i]);
                if (frand01() < 0.03) {
                    p->offset = rand() % (MAX_OFFSET + 1);
                    p->invert_cycle = (frand01() < ASCEND_INVERT_P);
                    p->state = STATE_ASCEND;
                    p->index = 0;
                }
                return;

            case STATE_ASCEND:
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->ascend, p->index), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                p->index++;
                if (p->index >= p->ascend.frame_count) {
                    p->state = choose_idle_state();
                    p->index = 0;
                    p->state_hold = IDLE_MIN_HOLD_FRAMES;
                }
                return;

            case STATE_DESCEND:
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->descend, p->index), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                p->index++;
                if (p->index >= p->descend.frame_count) {
                    p->state = STATE_HIDDEN;
                    p->index = 0;
                    p->invert_cycle = false;
                }
                return;

            case STATE_IDLE1:
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->idle1, 0), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                if (p->state_hold > 0) {
                    p->state_hold--;
                    return;
                }
                if (frand01() < IDLE_DESCEND_P) {
                    p->state = STATE_DESCEND;
                    p->index = 0;
                } else if (frand01() < IDLE_ANIM_P) {
                    p->state = choose_anim_state();
                    p->index = 0;
                }
                return;

            case STATE_IDLE2:
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->idle2, 0), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                if (p->state_hold > 0) {
                    p->state_hold--;
                    return;
                }
                if (frand01() < IDLE_DESCEND_P) {
                    p->state = STATE_DESCEND;
                    p->index = 0;
                } else if (frand01() < IDLE_ANIM_P) {
                    p->state = choose_anim_state();
                    p->index = 0;
                }
                return;

            case STATE_BLINK:
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->blink, p->index), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                p->index++;
                if (p->index >= p->blink.frame_count) {
                    p->state = choose_idle_state();
                    p->index = 0;
                }
                return;

            case STATE_BLINK_VARIANT:
                if (p->index == 0) {
                    overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->blink, 0), p->offset, out);
                    if (p->invert_cycle) {
                        flip_vertical(out, tmp);
                        memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                    }
                    if (frand01() < BLINK_VARIANT_ADVANCE_P) p->index = 1;
                    return;
                }
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(&p->blink, p->index), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                p->index++;
                if (p->index >= p->blink.frame_count) {
                    p->state = choose_idle_state();
                    p->index = 0;
                }
                return;

            case STATE_LOOK1:
            case STATE_LOOK2:
            case STATE_LOOK3: {
                const Asset *look = p->state == STATE_LOOK1 ? &p->look1 : (p->state == STATE_LOOK2 ? &p->look2 : &p->look3);
                size_t hold_idx = look->frame_count > 1 ? 1 : 0;
                if (p->index == 0) {
                    overlay_frame(asset_frame(&p->bg, 0), asset_frame(look, 0), p->offset, out);
                    if (p->invert_cycle) {
                        flip_vertical(out, tmp);
                        memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                    }
                    p->index = 1;
                    return;
                }
                overlay_frame(asset_frame(&p->bg, 0), asset_frame(look, hold_idx), p->offset, out);
                if (p->invert_cycle) {
                    flip_vertical(out, tmp);
                    memcpy(out, tmp, PIXELS_PER_FRAME * sizeof(Color));
                }
                if (frand01() < LOOK_CHAIN_P) {
                    p->state = choose_anim_state();
                    p->index = 0;
                }
                return;
            }
        }
    }
}

static bool load_packets(const char *path, Packet65 **out_packets, size_t *out_count) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "failed to open session file: %s\n", path);
        return false;
    }

    Packet65 *packets = NULL;
    size_t count = 0;
    size_t cap = 0;
    char line[4096];

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '[') continue;
        char *p = strchr(line, ']');
        if (!p) continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;

        Packet65 pkt = {0};
        unsigned value = 0;
        size_t idx = 0;
        while (*p && idx < REPORT_LEN) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '\r' || *p == '\n') break;
            if (sscanf(p, "%x", &value) != 1) break;
            pkt.data[idx++] = (unsigned char)value;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        }
        if (idx != REPORT_LEN) {
            fprintf(stderr, "bad packet line, expected 65 bytes got %zu\n", idx);
            free(packets);
            fclose(fp);
            return false;
        }
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 32;
            Packet65 *new_packets = (Packet65 *)realloc(packets, new_cap * sizeof(Packet65));
            if (!new_packets) {
                free(packets);
                fclose(fp);
                return false;
            }
            packets = new_packets;
            cap = new_cap;
        }
        packets[count++] = pkt;
    }

    fclose(fp);
    *out_packets = packets;
    *out_count = count;
    return true;
}

static bool query_caps_match(HANDLE h) {
    PHIDP_PREPARSED_DATA prep = NULL;
    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(h, &prep)) return false;
    NTSTATUS st = HidP_GetCaps(prep, &caps);
    HidD_FreePreparsedData(prep);
    if (st != HIDP_STATUS_SUCCESS) return false;
    return caps.InputReportByteLength == REPORT_LEN && caps.OutputReportByteLength == REPORT_LEN;
}

static HANDLE open_target_device(char *chosen_path, size_t chosen_path_len) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "SetupDiGetClassDevs failed\n");
        return INVALID_HANDLE_VALUE;
    }

    HANDLE result = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, NULL, &hid_guid, index, &if_data); ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &if_data, NULL, 0, &needed, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &if_data, detail, needed, NULL, NULL)) {
            free(detail);
            continue;
        }

        const char *path = detail->DevicePath;
        if (!ascii_icontains(path, "VID_0C45") ||
            !ascii_icontains(path, "PID_800A") ||
            !ascii_icontains(path, TARGET_MI)) {
            free(detail);
            continue;
        }

        HANDLE h = CreateFileA(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        if (h == INVALID_HANDLE_VALUE) {
            free(detail);
            continue;
        }

        HIDD_ATTRIBUTES attrs;
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(h, &attrs) ||
            attrs.VendorID != TARGET_VID ||
            attrs.ProductID != TARGET_PID ||
            !query_caps_match(h)) {
            CloseHandle(h);
            free(detail);
            continue;
        }

        strncpy(chosen_path, path, chosen_path_len - 1);
        chosen_path[chosen_path_len - 1] = '\0';
        result = h;
        free(detail);
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
    return result;
}

static void improve_priority(void) {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

static unsigned int generate_markov_seed(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    unsigned long long mix = GetTickCount64();
    mix ^= (unsigned long long)GetCurrentProcessId() << 16;
    mix ^= (unsigned long long)GetCurrentThreadId() << 1;
    mix ^= (unsigned long long)counter.LowPart;
    mix ^= (unsigned long long)counter.HighPart << 32;
    mix ^= mix >> 33;
    mix *= 0xff51afd7ed558ccdULL;
    mix ^= mix >> 33;
    return (unsigned int)(mix ^ (mix >> 32));
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_stop_requested, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

static bool send_session_quiet(HANDLE h, const Packet65 *packets, size_t count, DWORD delay_ms, bool quiet) {
    for (size_t i = 0; i < count; ++i) {
        DWORD written = 0;
        if (!WriteFile(h, packets[i].data, REPORT_LEN, &written, NULL)) {
            fprintf(stderr, "[%02zu] WriteFile failed: %lu\n", i, GetLastError());
            return false;
        }
        if (!quiet) {
            printf("[%02zu] wrote=%lu\n", i, (unsigned long)written);
        }
        Sleep(delay_ms);
    }
    return true;
}

static bool send_session(HANDLE h, const Packet65 *packets, size_t count, DWORD delay_ms) {
    return send_session_quiet(h, packets, count, delay_ms, false);
}

int main(int argc, char **argv) {
    char exe_dir[MAX_PATH] = {0};
    char default_session_path[MAX_PATH * 2] = {0};
    char default_long_animation_dir[MAX_PATH * 2] = {0};
    get_exe_dir(exe_dir, sizeof(exe_dir));
    join_path2(default_session_path, sizeof(default_session_path), exe_dir, "test-led.session_packets.txt");
    join_path2(default_long_animation_dir, sizeof(default_long_animation_dir), exe_dir, "long_animation");

    const char *session_path = default_session_path;
    DWORD delay_ms = DEFAULT_DELAY_MS;
    DWORD session_gap_ms = DEFAULT_SESSION_GAP_MS;
    bool hold = false;
    bool markov = false;
    bool quiet = false;
    DWORD frame_delay_ms = DEFAULT_MARKOV_FRAME_DELAY_MS;
    unsigned int seed = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--hold") == 0) {
            hold = true;
        } else if (strcmp(argv[i], "--markov") == 0) {
            markov = true;
            hold = true;
        } else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            delay_ms = (DWORD)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--session-gap-ms") == 0 && i + 1 < argc) {
            session_gap_ms = (DWORD)strtoul(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 64;
        } else if (positional == 0) {
            session_path = argv[i];
            positional++;
        } else if (positional == 1) {
            delay_ms = (DWORD)strtoul(argv[i], NULL, 10);
            positional++;
        } else {
            fprintf(stderr, "too many positional arguments\n");
            return 64;
        }
    }

    improve_priority();
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    srand(seed);

    char chosen_path[1024] = {0};
    HANDLE h = open_target_device(chosen_path, sizeof(chosen_path));
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "failed to find/open target HID interface\n");
        return 2;
    }

    printf("device_path=%s\n", chosen_path);
    printf("delay_ms=%lu\n", (unsigned long)delay_ms);
    printf("hold=%s\n", hold ? "true" : "false");
    printf("session_gap_ms=%lu\n", (unsigned long)session_gap_ms);
    printf("markov=%s\n", markov ? "true" : "false");
    printf("quiet=%s\n", quiet ? "true" : "false");

    if (markov) {
        quiet = true;
        seed = generate_markov_seed();
        printf("frame_delay_ms=%lu\n", (unsigned long)frame_delay_ms);
        printf("seed=%u\n", seed);
        MarkovPlayer player;
        Color frame[PIXELS_PER_FRAME];
        Packet65 session[19];
        if (!init_markov_player(&player, default_long_animation_dir)) {
            CloseHandle(h);
            return 4;
        }
        printf("markov active; press Ctrl+C to stop.\n");
        while (!InterlockedCompareExchange(&g_stop_requested, 0, 0)) {
            render_markov_next_frame(&player, frame);
            build_session_packets_from_frame(frame, session);
            if (!send_session_quiet(h, session, 19, delay_ms, quiet)) {
                free_markov_player(&player);
                CloseHandle(h);
                return 3;
            }
            if (frame_delay_ms > 0) Sleep(frame_delay_ms);
        }
        free_markov_player(&player);
        CloseHandle(h);
        return 0;
    }

    Packet65 *packets = NULL;
    size_t packet_count = 0;
    if (!load_packets(session_path, &packets, &packet_count)) {
        CloseHandle(h);
        return 1;
    }

    printf("session_file=%s\n", session_path);
    printf("packet_count=%zu\n", packet_count);

    bool ok = true;
    if (!hold) {
        ok = send_session_quiet(h, packets, packet_count, delay_ms, quiet);
    } else {
        printf("hold active; press Ctrl+C to stop.\n");
        while (!InterlockedCompareExchange(&g_stop_requested, 0, 0)) {
            ok = send_session_quiet(h, packets, packet_count, delay_ms, quiet);
            if (!ok) break;
            if (session_gap_ms > 0) Sleep(session_gap_ms);
        }
    }
    CloseHandle(h);
    free(packets);
    return ok ? 0 : 3;
}
