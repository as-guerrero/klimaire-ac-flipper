// Klimaire PTAC remote for Flipper Zero
// Stateful remote for Klimaire KTHN-series PTAC units (Midea-built).
// Two candidate Midea IR protocols: A = Midea 48-bit (native F), B = Coolix 24-bit (C).
//
// Controls:
//   Up / Down      temperature +1 / -1 F (sends)
//   Left / Right   mode: Cool / Heat / Fan (sends)
//   OK             power on / off (sends)
//   Hold OK        cycle fan speed Auto/Low/Med/High (sends)
//   Hold Left      switch protocol A / B (no send)
//   Back           exit (settings are saved)

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <infrared_transmit.h>
#include <notification/notification_messages.h>
#include <toolbox/saved_struct.h>
#include <storage/storage.h>

#define TAG "KlimaireRemote"

#define SAVE_PATH    APP_DATA_PATH("state.bin")
#define SAVE_MAGIC   0x4B
#define SAVE_VERSION 1

#define TEMP_MIN_F 62
#define TEMP_MAX_F 86

typedef enum { ModeCool, ModeHeat, ModeFan, ModeCount } AcMode;
typedef enum { FanAuto, FanLow, FanMed, FanHigh, FanCount } AcFan;
typedef enum { ProtoA, ProtoB, ProtoCount } AcProto;

typedef struct {
    uint8_t power;
    uint8_t mode;
    uint8_t fan;
    uint8_t temp_f;
    uint8_t proto;
} AcState;

typedef struct {
    AcState state;
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    uint32_t sent_count;
} App;

static const char* mode_names[ModeCount] = {"COOL", "HEAT", "FAN"};
static const char* fan_names[FanCount] = {"Auto", "Low", "Med", "High"};
static const char* proto_names[ProtoCount] = {"A", "B"};

// ---------- IR encoding ----------

#define MAX_TIMINGS 200

static uint8_t rev8(uint8_t b) {
    uint8_t r = 0;
    for(int i = 0; i < 8; i++) {
        r = (r << 1) | (b & 1);
        b >>= 1;
    }
    return r;
}

// Appends header + bits (MSB first) + trailing mark. Returns new count.
static size_t append_frame(
    uint32_t* t,
    size_t n,
    const uint8_t* bytes,
    size_t len,
    bool invert,
    uint32_t hdr,
    uint32_t mark,
    uint32_t one,
    uint32_t zero) {
    t[n++] = hdr;
    t[n++] = hdr;
    for(size_t i = 0; i < len; i++) {
        uint8_t b = invert ? (uint8_t)~bytes[i] : bytes[i];
        for(int bit = 7; bit >= 0; bit--) {
            t[n++] = mark;
            t[n++] = ((b >> bit) & 1) ? one : zero;
        }
    }
    t[n++] = mark;
    return n;
}

// Protocol A: Midea 48-bit, second frame bitwise inverted.
static size_t encode_midea48(const AcState* s, uint32_t* t) {
    static const uint8_t mode_map[ModeCount] = {0 /*cool*/, 3 /*heat*/, 4 /*fan*/};
    uint8_t b[6];
    b[0] = 0xA1;
    b[1] = (uint8_t)((s->power ? 0x80 : 0) | ((s->fan & 3) << 3) | mode_map[s->mode]);
    b[2] = (uint8_t)(0x20 | ((s->temp_f - TEMP_MIN_F) & 0x1F));
    b[3] = 0xFF;
    b[4] = 0xFF;
    uint8_t sum = 0;
    for(int i = 0; i < 5; i++) sum += rev8(b[i]);
    b[5] = rev8((uint8_t)(256 - sum));

    size_t n = 0;
    n = append_frame(t, n, b, 6, false, 4480, 560, 1680, 560);
    t[n++] = 5600;
    n = append_frame(t, n, b, 6, true, 4480, 560, 1680, 560);
    return n;
}

// Protocol B: Midea/Coolix 24-bit, each byte followed by its complement, frame sent twice.
static size_t encode_coolix(const AcState* s, uint32_t* t) {
    static const uint8_t temp_code[14] = {
        0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4, 0xC, 0xD, 0x9, 0x8, 0xA, 0xB}; // 17..30 C
    static const uint8_t fan_code[FanCount] = {0xBF, 0x9F, 0x5F, 0x3F};
    uint8_t b1, b2;
    if(!s->power) {
        b1 = 0x7B;
        b2 = 0xE0;
    } else if(s->mode == ModeFan) {
        b1 = fan_code[s->fan == FanAuto ? FanLow : s->fan];
        b2 = 0xE4;
    } else {
        int c = ((int)s->temp_f - 32) * 5;
        c = (c + (c >= 0 ? 4 : -4)) / 9; // round to nearest
        if(c < 17) c = 17;
        if(c > 30) c = 30;
        b1 = fan_code[s->fan];
        b2 = (uint8_t)((temp_code[c - 17] << 4) | (s->mode == ModeHeat ? 0xC : 0x0));
    }
    uint8_t bytes[6] = {0xB2, 0x4D, b1, (uint8_t)~b1, b2, (uint8_t)~b2};
    size_t n = 0;
    n = append_frame(t, n, bytes, 6, false, 4418, 553, 1659, 553);
    t[n++] = 5217;
    n = append_frame(t, n, bytes, 6, false, 4418, 553, 1659, 553);
    return n;
}

static void send_state(App* app) {
    uint32_t* timings = malloc(sizeof(uint32_t) * MAX_TIMINGS);
    AcState s = app->state;
    size_t n = (s.proto == ProtoA) ? encode_midea48(&s, timings) : encode_coolix(&s, timings);
    infrared_send_raw_ext(timings, n, true, 38000, 0.33f);
    free(timings);
    app->sent_count++;

    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notif, &sequence_blink_magenta_10);
    furi_record_close(RECORD_NOTIFICATION);
}

// ---------- Persistence ----------

static void state_defaults(AcState* s) {
    s->power = 0;
    s->mode = ModeCool;
    s->fan = FanAuto;
    s->temp_f = 72;
    s->proto = ProtoA;
}

static void state_load(AcState* s) {
    if(!saved_struct_load(SAVE_PATH, s, sizeof(AcState), SAVE_MAGIC, SAVE_VERSION) ||
       s->mode >= ModeCount || s->fan >= FanCount || s->proto >= ProtoCount ||
       s->temp_f < TEMP_MIN_F || s->temp_f > TEMP_MAX_F) {
        state_defaults(s);
    }
}

static void state_save(const AcState* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, APP_DATA_PATH(""));
    furi_record_close(RECORD_STORAGE);
    saved_struct_save(SAVE_PATH, (void*)s, sizeof(AcState), SAVE_MAGIC, SAVE_VERSION);
}

// ---------- UI ----------

static void draw_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    AcState s = app->state;
    furi_mutex_release(app->mutex);

    char buf[32];
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Klimaire PTAC");
    snprintf(buf, sizeof(buf), "Proto %s", proto_names[s.proto]);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    // Power badge
    canvas_set_font(canvas, FontPrimary);
    if(s.power) {
        canvas_draw_rbox(canvas, 2, 18, 28, 14, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 16, 25, AlignCenter, AlignCenter, "ON");
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, 2, 18, 28, 14, 3);
        canvas_draw_str_aligned(canvas, 16, 25, AlignCenter, AlignCenter, "OFF");
    }

    // Mode and fan
    canvas_draw_str(canvas, 2, 46, mode_names[s.mode]);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Fan: %s", fan_names[s.fan]);
    canvas_draw_str(canvas, 2, 57, buf);

    // Temperature
    if(s.mode == ModeFan) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 92, 34, AlignCenter, AlignCenter, "fan only");
    } else {
        canvas_set_font(canvas, FontBigNumbers);
        snprintf(buf, sizeof(buf), "%u", s.temp_f);
        canvas_draw_str_aligned(canvas, 88, 34, AlignCenter, AlignCenter, buf);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 108, 28, "F");
        canvas_draw_circle(canvas, 105, 21, 1);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "hold OK: fan");
}

static void input_callback(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

// ---------- Entry ----------

int32_t klimaire_remote_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->sent_count = 0;
    state_load(&app->state);

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_callback, app);
    view_port_input_callback_set(vp, input_callback, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        bool send = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        AcState* s = &app->state;

        if(ev.type == InputTypeShort || ev.type == InputTypeRepeat) {
            switch(ev.key) {
            case InputKeyUp:
                if(s->temp_f < TEMP_MAX_F) s->temp_f++;
                if(s->mode != ModeFan) {
                    s->power = 1;
                    send = true;
                }
                break;
            case InputKeyDown:
                if(s->temp_f > TEMP_MIN_F) s->temp_f--;
                if(s->mode != ModeFan) {
                    s->power = 1;
                    send = true;
                }
                break;
            default:
                break;
            }
        }
        if(ev.type == InputTypeShort) {
            switch(ev.key) {
            case InputKeyRight:
                s->mode = (s->mode + 1) % ModeCount;
                s->power = 1;
                send = true;
                break;
            case InputKeyLeft:
                s->mode = (s->mode + ModeCount - 1) % ModeCount;
                s->power = 1;
                send = true;
                break;
            case InputKeyOk:
                s->power = !s->power;
                send = true;
                break;
            case InputKeyBack:
                running = false;
                break;
            default:
                break;
            }
        } else if(ev.type == InputTypeLong) {
            if(ev.key == InputKeyOk) {
                s->fan = (s->fan + 1) % FanCount;
                s->power = 1;
                send = true;
            } else if(ev.key == InputKeyLeft) {
                s->proto = (s->proto + 1) % ProtoCount;
            }
        }
        furi_mutex_release(app->mutex);

        if(send) send_state(app);
        view_port_update(vp);
    }

    state_save(&app->state);

    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
