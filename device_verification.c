// ===== usb_touch_screen_libevdev_fixed_send_match_event.c =====
// Key changes: added find_event_for_libusb_device() to match /dev/input/eventN
// to the same physical USB device opened by libusb (by matching idVendor/idProduct and optional serial).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <libusb-1.0/libusb.h>
#include <libevdev/libevdev.h>

#include "usb_monitor_control.h"

#define WIDTH   800
#define HEIGHT  480
#define RECT_W  60
#define RECT_H  60
#define COLOR_RECT 0xF800
#define COLOR_BG   0x0000

#define VENDOR_ID  0x1fc9
#define PRODUCT_ID 0x8335
#define EP_OUT     0x03
#define PACKET_SIZE 1024

#define USB_TOUCH_INTERFACE_NUM  0
#define USB_SCREEN_INTERFACE_NUM 1
#define TOUCH_EVENT_SCAN_MAX     64   // scan this many event nodes (0..63)

#define AUTO_RANDOM_MOVE

#ifdef AUTO_RANDOM_MOVE
static uint64_t last_user_input_us = 0;   // 마지막 실제 터치 시각
static uint64_t last_auto_gen_us   = 0;   // 마지막 랜덤 타겟 생성 시각
#endif

static uint16_t framebuffer[HEIGHT][WIDTH];

static inline void clear_framebuffer(void) {
    for (int y=0;y<HEIGHT;y++) for (int x=0;x<WIDTH;x++) framebuffer[y][x]=COLOR_BG;
}
typedef struct { int x,y; } Rect;
static inline void draw_rectangle(const Rect* r) {
    for (int yy=0; yy<RECT_H; yy++){
        int py = r->y + yy;
        if (py<0 || py>=HEIGHT) continue;
        for (int xx=0; xx<RECT_W; xx++){
            int px = r->x + xx;
            if (px<0 || px>=WIDTH) continue;
            framebuffer[py][px]=COLOR_RECT;
        }
    }
}
static inline void clamp_rect(Rect *r){
    if (r->x < 0) r->x = 0;
    if (r->y < 0) r->y = 0;
    if (r->x + RECT_W >= WIDTH)  r->x = WIDTH - RECT_W;
    if (r->y + RECT_H >= HEIGHT) r->y = HEIGHT - RECT_H;
}

/* libusb state */
static libusb_context *ctx = NULL;
static libusb_device_handle *handle = NULL;
static int interface_claimed_screen = 0;
static int kernel_attached_screen = 0;

/* libevdev touch */
typedef struct { struct libevdev *dev; int fd; } touch_device_info_t;
static touch_device_info_t touch_info = { NULL, -1 };
typedef struct {
    int have_mt, have_st;
    int abs_min_x, abs_max_x;
    int abs_min_y, abs_max_y;
    int last_x,last_y;
    int has_pos;
    #ifdef AUTO_RANDOM_MOVE
    int updated;   // <<< ADDED: 새 좌표가 이 프레임에 갱신되었는지 표시
    #endif

} touch_state_t;
static touch_state_t g_touch = {0};

/* helper prototypes (defined below) */
static int connect_device(void);
static int send_frame_sync(libusb_device_handle *h);

/* ---------- functions for matching event node to libusb device ---------- */

/* read small file into buffer (trim newline) */
static int read_file_to_buf(const char *path, char *buf, size_t sz) {
    if (!path || !buf) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)sz, f)) { fclose(f); return -1; }
    // trim newline
    size_t L = strlen(buf);
    while (L > 0 && (buf[L-1] == '\n' || buf[L-1] == '\r')) { buf[L-1] = '\0'; L--; }
    fclose(f);
    return 0;
}

/*
 * Try to find /dev/input/eventN which belongs to the same USB device as the libusb handle.
 * We compare idVendor/idProduct and, if available, iSerialNumber (string desc).
 *
 * out_event_path must have size at least PATH_MAX.
 * Returns 0 on success and writes "/dev/input/eventN" into out_event_path. -1 on failure.
 */
static int find_event_for_libusb_device(libusb_device_handle *h, char *out_event_path, size_t out_sz) {
    if (!h || !out_event_path) return -1;

    libusb_device *dev = libusb_get_device(h);
    if (!dev) return -1;

    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) < 0) return -1;

    // optional serial string
    char serial_str[256] = {0};
    if (desc.iSerialNumber) {
        if (libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                              (unsigned char*)serial_str, sizeof(serial_str)) < 0) {
            serial_str[0] = '\0';
        }
    }

    // vendor/product from descriptor (hex)
    unsigned short vid = desc.idVendor;
    unsigned short pid = desc.idProduct;

    // iterate /dev/input/eventN
    for (int i = 0; i < TOUCH_EVENT_SCAN_MAX; ++i) {
        char devnode[64]; snprintf(devnode, sizeof(devnode), "/dev/input/event%d", i);
        // if node doesn't exist skip
        if (access(devnode, R_OK) != 0) continue;

        // resolve sysfs path for this event: /sys/class/input/eventN/device (resolve symlink)
        char syslink[PATH_MAX];
        snprintf(syslink, sizeof(syslink), "/sys/class/input/event%d/device", i);

        char resolved[PATH_MAX];
        if (!realpath(syslink, resolved)) continue;

        // walk up directories until we find idVendor && idProduct files (or root)
        char cur[PATH_MAX];
        strncpy(cur, resolved, sizeof(cur));
        cur[sizeof(cur)-1] = '\0';
        bool matched = false;
        while (1) {
            // build candidate idVendor path
            char vid_path[PATH_MAX], pid_path[PATH_MAX], serial_path[PATH_MAX];
            snprintf(vid_path, sizeof(vid_path), "%s/idVendor", cur);
            snprintf(pid_path, sizeof(pid_path), "%s/idProduct", cur);
            snprintf(serial_path, sizeof(serial_path), "%s/serial", cur);

            if (access(vid_path, R_OK) == 0 && access(pid_path, R_OK) == 0) {
                char vbuf[64], pbuf[64];
                if (read_file_to_buf(vid_path, vbuf, sizeof(vbuf)) < 0) break;
                if (read_file_to_buf(pid_path, pbuf, sizeof(pbuf)) < 0) break;
                unsigned int v = (unsigned int)strtoul(vbuf, NULL, 16);
                unsigned int p = (unsigned int)strtoul(pbuf, NULL, 16);
                if ((unsigned int)vid == v && (unsigned int)pid == p) {
                    // if serial is available from libusb, try to match sysfs 'serial' if present
                    if (serial_str[0] != '\0' && access(serial_path, R_OK) == 0) {
                        char sfs[256] = {0};
                        if (read_file_to_buf(serial_path, sfs, sizeof(sfs)) == 0) {
                            if (strcmp(serial_str, sfs) == 0) {
                                matched = true; // vendor/pid and serial match
                            } else {
                                matched = false;
                            }
                        } else {
                            matched = false;
                        }
                    } else {
                        // serial not available or not present; accept vendor/pid match
                        matched = true;
                    }
                }
                // either way, break from loop to check matched flag
                break;
            }

            // go one level up
            char *slash = strrchr(cur, '/');
            if (!slash || slash == cur) break;
            *slash = '\0';
        } // end walk up

        if (matched) {
            // success: return devnode
            strncpy(out_event_path, devnode, out_sz);
            out_event_path[out_sz-1] = '\0';
            return 0;
        }
    }
    return -1;
}

/* ---------- existing helpers (open/close/autodetect fallback etc.) ---------- */
static int open_touch_device_by_libevdev(touch_device_info_t *out, const char *path) {
    if (!out || !path) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); return -1; }
    out->fd = fd; out->dev = dev; return 0;
}
static void close_touch_device_by_libevdev(touch_device_info_t *t) {
    if (!t) return;
    if (t->dev) libevdev_free(t->dev);
    if (t->fd >= 0) close(t->fd);
    t->dev = NULL; t->fd = -1;
}
static int autodetect_touch_event_path(char *out_path, size_t out_sz) {
    // kept as fallback: checks for any event node with ABS capabilities
    for (int i = 0; i < TOUCH_EVENT_SCAN_MAX; i++) {
        char p[64]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        if (access(p, R_OK) != 0) continue;
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct libevdev *d = NULL;
        if (libevdev_new_from_fd(fd, &d) == 0) {
            bool ok = libevdev_has_event_type(d, EV_ABS) &&
                      ((libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_X) &&
                        libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_Y)) ||
                       (libevdev_has_event_code(d, EV_ABS, ABS_X) &&
                        libevdev_has_event_code(d, EV_ABS, ABS_Y)));
            if (ok) {
                strncpy(out_path, p, out_sz); out_path[out_sz-1] = '\0';
                libevdev_free(d); close(fd); return 0;
            }
            libevdev_free(d);
        }
        close(fd);
    }
    return -1;
}

/* init touch caps from libevdev device */
static void init_touch_caps_from_dev(struct libevdev *dev) {
    memset(&g_touch,0,sizeof(g_touch));
    g_touch.have_mt = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X) &&
                      libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y);
    g_touch.have_st = libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
                      libevdev_has_event_code(dev, EV_ABS, ABS_Y);
    const struct input_absinfo *ax=NULL,*ay=NULL;
    if (g_touch.have_mt) { ax = libevdev_get_abs_info(dev, ABS_MT_POSITION_X); ay = libevdev_get_abs_info(dev, ABS_MT_POSITION_Y); }
    else if (g_touch.have_st) { ax = libevdev_get_abs_info(dev, ABS_X); ay = libevdev_get_abs_info(dev, ABS_Y); }
    if (ax && ay) {
        g_touch.abs_min_x = ax->minimum; g_touch.abs_max_x = ax->maximum;
        g_touch.abs_min_y = ay->minimum; g_touch.abs_max_y = ay->maximum;
    } else {
        g_touch.abs_min_x = 0; g_touch.abs_max_x = 65535;
        g_touch.abs_min_y = 0; g_touch.abs_max_y = 65535;
    }
    g_touch.has_pos = 0;
}

/* update_touch_from_event same as before */
static void update_touch_from_event(const struct input_event *ev) {
    if (!ev) return;
    static int cur_ax=-1, cur_ay=-1;
    if (g_touch.have_mt) {
        if (ev->type == EV_ABS) {
            if (ev->code == ABS_MT_POSITION_X) cur_ax = ev->value;
            else if (ev->code == ABS_MT_POSITION_Y) cur_ay = ev->value;
        } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            if (cur_ax>=0 && cur_ay>=0) {
                int sx = (int)((long long)(cur_ax - g_touch.abs_min_x) * (WIDTH-1) / (g_touch.abs_max_x - g_touch.abs_min_x));
                int sy = (int)((long long)(cur_ay - g_touch.abs_min_y) * (HEIGHT-1) / (g_touch.abs_max_y - g_touch.abs_min_y));
                g_touch.last_x = sx; g_touch.last_y = sy; g_touch.has_pos = 1;
                #ifdef AUTO_RANDOM_MOVE
                g_touch.updated = 1;   // <<< ADDED
                #endif
                cur_ax = cur_ay = -1;
            }
        }
    } else if (g_touch.have_st) {
        if (ev->type == EV_ABS) {
            if (ev->code == ABS_X) cur_ax = ev->value;
            else if (ev->code == ABS_Y) cur_ay = ev->value;
        } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            if (cur_ax>=0 && cur_ay>=0) {
                int sx = (int)((long long)(cur_ax - g_touch.abs_min_x) * (WIDTH-1) / (g_touch.abs_max_x - g_touch.abs_min_x));
                int sy = (int)((long long)(cur_ay - g_touch.abs_min_y) * (HEIGHT-1) / (g_touch.abs_max_y - g_touch.abs_min_y));
                g_touch.last_x = sx; g_touch.last_y = sy; g_touch.has_pos = 1;
                #ifdef AUTO_RANDOM_MOVE
                g_touch.updated = 1;   // <<< ADDED
                #endif
                cur_ax = cur_ay = -1;
            }
        }
    }
}


/* ====== send_frame_sync (chunked) as before ====== */
static int send_frame_sync(libusb_device_handle *h) {
    if (!h) return LIBUSB_ERROR_NO_DEVICE;
    const int total_bytes = WIDTH * HEIGHT * 2;
    uint8_t *data = (uint8_t*)framebuffer;
    int offset = 0;
    int timeout_ms = 1000;
    while (offset < total_bytes) {
        int chunk = total_bytes - offset;
        if (chunk > PACKET_SIZE) chunk = PACKET_SIZE;
        int transferred = 0;
        int r = libusb_bulk_transfer(h, EP_OUT, data + offset, chunk, &transferred, timeout_ms);
        if (r == LIBUSB_ERROR_NO_DEVICE) return LIBUSB_ERROR_NO_DEVICE;
        if (r != 0) {
            fprintf(stderr, "libusb_bulk_transfer error at offset %d: %s (%d)\n", offset, libusb_error_name(r), r);
            return r;
        }
        if (transferred <= 0) {
            fprintf(stderr, "libusb_bulk_transfer transferred 0 at offset %d\n", offset);
            return -1;
        }
        offset += transferred;
    }
    return 0;
}


/* Replace the old check_usb_device_disconnected() with this enhanced version.
 *
 * Logic:
 * - If handle is NULL -> consider disconnected.
 * - Get libusb_device* from handle; read its bus number and port numbers (path).
 * - Enumerate current libusb device list and try to find a device with same bus and same port-path.
 * - If not found -> device physically disconnected.
 * - Also treat libusb_get_device_descriptor() failures and libusb_handle_events_timeout_completed()
 *   returning LIBUSB_ERROR_NO_DEVICE as disconnect.
 *
 * This is more robust in scenarios where the handle exists but the physical device has been unplugged.
 */
static bool check_usb_device_disconnected(void) {
    if (!ctx) return true;            // sanity
    if (!handle) {
        // No handle -> definitely disconnected
        return true;
    }

    libusb_device *dev = libusb_get_device(handle);
    if (!dev) {
        // cannot obtain device from handle -> treat as disconnected
        return true;
    }

    // 1) Quick descriptor check: if libusb reports NO_DEVICE here, it's gone.
    struct libusb_device_descriptor desc;
    int r = libusb_get_device_descriptor(dev, &desc);
    if (r == LIBUSB_ERROR_NO_DEVICE) {
        // physical device gone
        return true;
    }
    // If other error, continue to deeper checks (we don't fail on other small errors)

    // 2) Get bus number and port numbers (port path)
    uint8_t ports_saved[16];
    int depth_saved = libusb_get_port_numbers(dev, ports_saved, sizeof(ports_saved));
    uint8_t bus_saved = libusb_get_bus_number(dev);

    // If we couldn't obtain port numbers (depth <= 0), fall back to descriptor-based check:
    // iterate device list and check for any device with same vid/pid on same bus.
    libusb_device **devs = NULL;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        // can't list devices -> conservative: ask libusb event loop if it says device gone
        struct timeval tv = {0, 0};
        int hr = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        return (hr == LIBUSB_ERROR_NO_DEVICE);
    }

    bool found = false;
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device *d = devs[i];

        // quick bus check
        if (libusb_get_bus_number(d) != bus_saved) continue;

        if (depth_saved > 0) {
            // compare port-number path
            uint8_t ports_cur[16];
            int depth_cur = libusb_get_port_numbers(d, ports_cur, sizeof(ports_cur));
            if (depth_cur == depth_saved) {
                bool match = true;
                for (int j = 0; j < depth_saved; ++j) {
                    if (ports_saved[j] != ports_cur[j]) { match = false; break; }
                }
                if (match) { found = true; break; }
            }
        } else {
            // fallback: match by idVendor/idProduct and bus + device address
            struct libusb_device_descriptor ddesc;
            if (libusb_get_device_descriptor(d, &ddesc) < 0) continue;
            if (ddesc.idVendor == VENDOR_ID && ddesc.idProduct == PRODUCT_ID) {
                // bus matches already; consider this a match (best-effort)
                found = true;
                break;
            }
        }
    }

    libusb_free_device_list(devs, 1);

    if (!found) {
        // device not present in system device list -> disconnected
        return true;
    }

    // 3) Final guard: still run event pump to check USB stack errors
    struct timeval tv = {0, 0};
    int hr = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    if (hr == LIBUSB_ERROR_NO_DEVICE) {
        return true;
    }

    // device still present
    return false;
}


static void LIBUSB_CALL transfer_callback(struct libusb_transfer *transfer) {
    //printf("Callback Transfer completed: %d bytes\n", transfer->actual_length);
}
struct libusb_transfer *transfer = NULL;
static int send_frame(libusb_device_handle *handle) {
    int transferred;
    int total_bytes = WIDTH * HEIGHT * 2; // RGB565 = 2 bytes/pixel
    uint8_t *data = (uint8_t*)framebuffer;
    int offset = 0;

    /*
    while (offset < total_bytes) {
        int chunk = (total_bytes - offset > PACKET_SIZE) ? PACKET_SIZE : (total_bytes - offset);
        int r = libusb_bulk_transfer(handle, EP_OUT, data + offset, chunk, &transferred, 1000);
        if (r != 0) {
            fprintf(stderr, "Bulk transfer error: %s\n", libusb_error_name(r));
            return r;
        }
        offset += transferred;
    }
*/
    //printf("trying to send\n");

    if(transfer) {
        if(transfer->length==transfer->actual_length) {
            //printf("Transfer completed, status: %d, length:%d actual_length:%d\n", transfer->status, transfer->length, transfer->actual_length);
            libusb_free_transfer(transfer);
        }
        else {
            printf("Transfer not completed, status: %d, length:%d actual_length:%d\n", transfer->status,transfer->length, transfer->actual_length);
            if(check_usb_device_disconnected()){
                return LIBUSB_ERROR_NO_DEVICE;
            }
            else return 0;
        }
    }

    transfer = libusb_alloc_transfer(0);
    if (!transfer) {

        //혹시 disconnect때문에 발생한것인지 check한다.
        struct timeval tv = {0, 0};
        int r=libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if(r==LIBUSB_ERROR_NO_DEVICE)return r;
        else {
            fprintf(stderr, "Failed to allocate transfer\n");
            return 1;
        }
    }
    //printf("handle:%p transfer:%p, EP_OUT:0x%x, total_bytes:%d\n", handle, transfer, EP_OUT, total_bytes);
    libusb_fill_bulk_transfer(transfer, handle, EP_OUT,
                            data, total_bytes,
                              transfer_callback, 0, 1000);
    int r = libusb_submit_transfer(transfer);
    if (r < 0) {
        fprintf(stderr, "Submit transfer error: %s(%d) \n", libusb_error_name(r),r);
        libusb_free_transfer(transfer);
        return r;
    }


    return 0;
}


/* libusb connect (first matching device) */
static int connect_device_inner(void) {
    libusb_device **devs = NULL;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) return (int)cnt;
    libusb_device *target = NULL;
    for (ssize_t i=0;i<cnt;i++){
        libusb_device *d = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(d, &desc) < 0) continue;
        if (desc.idVendor == VENDOR_ID && desc.idProduct == PRODUCT_ID) { libusb_ref_device(d); target = d; break; }
    }
    libusb_free_device_list(devs, 1);
    if (!target) return 1;
    if (handle) {
        if (interface_claimed_screen) {
            libusb_release_interface(handle, USB_SCREEN_INTERFACE_NUM);
            interface_claimed_screen = 0;
            if (kernel_attached_screen) { libusb_attach_kernel_driver(handle, USB_SCREEN_INTERFACE_NUM); kernel_attached_screen = 0; }
        }
        libusb_close(handle); handle = NULL;
    }
    int r = libusb_open(target, &handle);
    libusb_unref_device(target);
    if (r != 0) { fprintf(stderr,"libusb_open failed: %s (%d)\n", libusb_error_name(r), r); return 1; }
    if (libusb_kernel_driver_active(handle, USB_SCREEN_INTERFACE_NUM) == 1) {
        if (libusb_detach_kernel_driver(handle, USB_SCREEN_INTERFACE_NUM) == 0) kernel_attached_screen = 1;
        else kernel_attached_screen = 0;
    }
    if (libusb_claim_interface(handle, USB_SCREEN_INTERFACE_NUM) < 0) {
        fprintf(stderr, "Failed to claim screen interface %d\n", USB_SCREEN_INTERFACE_NUM);
        libusb_close(handle); handle = NULL; return 1;
    }
    interface_claimed_screen = 1;
    // optional offset reset
    usb_monitor_control_response_t resp;
    (void)libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        SCREEN_REQUEST_TYPE_OFFSET_RESET, 0, USB_SCREEN_INTERFACE_NUM,
        (unsigned char *)&resp, sizeof(resp), 500);
    return 0;
}
static volatile sig_atomic_t keep_running = 1;
static void handle_signal(int sig) { (void)sig; keep_running = 0; }
static int connect_device(void) {
    while (keep_running) {
        int r = connect_device_inner();
        if (r == 0) return 0;
        fprintf(stderr, "Waiting for 1fc9:8335 device (connect_device)...\n");
        sleep(1);
    }
    return 1;
}

/* time util */
static inline uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}


/* main */
int main(int argc, char *argv[]) {
    const char *explicit_event_path = NULL;
    if (argc >= 2) explicit_event_path = argv[1];

    signal(SIGINT, handle_signal); signal(SIGTERM, handle_signal);

    #ifdef AUTO_RANDOM_MOVE
    srand((unsigned)time(NULL));          // <<< ADDED: rand() 시드
    #endif

    if (libusb_init(&ctx) < 0) { fprintf(stderr,"libusb init failed\n"); return 1; }
    if (connect_device() != 0) { fprintf(stderr,"Device connect failed\n"); libusb_exit(ctx); return 1; }
    printf("Connected to USB screen on device 1fc9:8335\n");

    char event_path[PATH_MAX] = {0};
    // first try to find the event node belonging to the same libusb device
    if (find_event_for_libusb_device(handle, event_path, sizeof(event_path)) == 0) {
        printf("Found matching event node for this USB device: %s\n", event_path);
    } else if (explicit_event_path) {
        strncpy(event_path, explicit_event_path, sizeof(event_path)-1);
        event_path[sizeof(event_path)-1] = '\0';
    } else {
        // fallback autodetect (any ABS-capable event)
        if (autodetect_touch_event_path(event_path, sizeof(event_path)) == 0) {
            printf("Autodetected touch event device (fallback): %s\n", event_path);
        } else {
            fprintf(stderr,"Failed to find touch event for device and fallback autodetect failed\n");
            if (interface_claimed_screen) { libusb_release_interface(handle, USB_SCREEN_INTERFACE_NUM); interface_claimed_screen=0; }
            if (handle) libusb_close(handle);
            libusb_exit(ctx);
            return 1;
        }
    }

    // open libevdev on found event node
    if (open_touch_device_by_libevdev(&touch_info, event_path) != 0) {
        perror("open_touch_device_by_libevdev");
        if (interface_claimed_screen) { libusb_release_interface(handle, USB_SCREEN_INTERFACE_NUM); interface_claimed_screen=0; }
        if (handle) libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }
    init_touch_caps_from_dev(touch_info.dev);
    printf("Touch device opened. have_mt=%d have_st=%d absX=[%d..%d] absY=[%d..%d]\n",
           g_touch.have_mt, g_touch.have_st, g_touch.abs_min_x, g_touch.abs_max_x, g_touch.abs_min_y, g_touch.abs_max_y);

    Rect rect = { (WIDTH-RECT_W)/2, (HEIGHT-RECT_H)/2 }, target_rect = rect;
    uint64_t last_frame = now_us();

    #ifdef AUTO_RANDOM_MOVE
    uint64_t now0 = now_us();             // <<< ADDED
    last_user_input_us = now0;            // <<< ADDED
    last_auto_gen_us   = now0;            // <<< ADDED
    #endif

    printf("Streaming frames; rectangle follows touch.\n");

    while (keep_running) {
        struct timeval tv = {0,0};
        int r = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            fprintf(stderr,"USB device disappeared; reconnecting...\n");
            if (connect_device() != 0) break;
            continue;
        } else if (r != 0) {
            fprintf(stderr,"libusb_handle_events error: %s (%d)\n", libusb_error_name(r), r);
            break;
        }

        #ifdef AUTO_RANDOM_MOVE
        g_touch.updated = 0;  // <<< ADDED: 이 프레임에서 새로운 터치 갱신 여부 초기화
        #endif

        struct input_event ev; int rc;
        do {
            rc = libevdev_next_event(touch_info.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) update_touch_from_event(&ev);
        } while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC);

        #ifdef AUTO_RANDOM_MOVE
        uint64_t now = now_us();   // <<< ADDED: 현재 시각

        if (g_touch.updated) {     // <<< CHANGED: has_pos 대신 updated 사용
            target_rect.x = g_touch.last_x - RECT_W/2;
            target_rect.y = g_touch.last_y - RECT_H/2;
            clamp_rect(&target_rect);
            last_user_input_us = now;      // <<< ADDED: 마지막 실제 입력 시각 갱신
        }

        // <<< ADDED: 3초 이상 실제 입력이 없으면, 3초마다 랜덤 위치를 새 타겟으로 설정
        const uint64_t AUTO_INTERVAL_US = 1000000ULL;  // 3초

        if (!g_touch.updated) {   // 이번 프레임에 실제 터치 갱신이 없을 때만 자동 입력 사용
            if (now - last_user_input_us >= AUTO_INTERVAL_US &&
                now - last_auto_gen_us   >= AUTO_INTERVAL_US) {
                int rx = rand() % (WIDTH  - RECT_W);
                int ry = rand() % (HEIGHT - RECT_H);
                target_rect.x = rx;
                target_rect.y = ry;
                clamp_rect(&target_rect);
                last_auto_gen_us = now;
                // printf("Auto target: (%d, %d)\n", rx, ry);
            }
        }
        #else
        if (g_touch.has_pos) {
            target_rect.x = g_touch.last_x - RECT_W/2;
            target_rect.y = g_touch.last_y - RECT_H/2;
            clamp_rect(&target_rect);
        }
        #endif

        rect.x += (target_rect.x - rect.x) / 4;
        rect.y += (target_rect.y - rect.y) / 4;
        clamp_rect(&rect);

        uint64_t elapsed = now_us() - last_frame;
        if (elapsed < 33000) {
            //printf("Frame delay %llu us; sleeping %llu us\n",
            //       (unsigned long long)elapsed, (unsigned long long)(33000 - elapsed));
            usleep((useconds_t)(33000 - elapsed));
        }
        else {
            //printf("Frame delay %llu us\n", (unsigned long long)elapsed);
        }

        last_frame = now_us();

        clear_framebuffer();
        draw_rectangle(&rect);

        if (!handle) {
             //연결이 끊어지면 재연결하지 않고 종료함.
            break;
            //fprintf(stderr,"Warning: handle NULL before send_frame_sync\n");
            //if (connect_device() != 0) break;
        }
        #if 0
        int sr = send_frame_sync(handle);
        #else
        struct timeval tvTmp0 = {0, 0};
        r=libusb_handle_events_timeout_completed(ctx, &tvTmp0, NULL); // usb 이벤트를 먼저 처리해줘야, 이전 transfer가 완료된것으로 libusb내부값이 변경된다.
        if(r==LIBUSB_ERROR_NO_DEVICE){
            //연결이 끊어지면 재연결하지 않고 종료함.
            break;
            //printf("USB device disconnected, reconnecting... A\n");
            //connect_device();
            //continue; // 재연결 후 루프 시작
        }
        else if(r!=0) {
            fprintf(stderr, "libusb_handle_events error: %s\n", libusb_error_name(r));
            break; // 에러 발생시 루프 종료
        }
        #endif

        int sr = send_frame(handle);
        if (sr == LIBUSB_ERROR_NO_DEVICE) {
             //연결이 끊어지면 재연결하지 않고 종료함.
             break;
            //fprintf(stderr,"Device gone during send; reconnecting...\n");
            //if (connect_device() != 0) break;
            //continue;
        } else if (sr != 0) {
            fprintf(stderr,"send_frame_sync returned %d\n", sr);
            break;
        }
    }

    /* cleanup */
    keep_running = 0;
    if (interface_claimed_screen && handle) {
        libusb_release_interface(handle, USB_SCREEN_INTERFACE_NUM);
        if (kernel_attached_screen) libusb_attach_kernel_driver(handle, USB_SCREEN_INTERFACE_NUM);
        interface_claimed_screen = 0; kernel_attached_screen = 0;
    }
    if (handle) libusb_close(handle);
    libusb_exit(ctx);

    close_touch_device_by_libevdev(&touch_info);
    return 0;
}
