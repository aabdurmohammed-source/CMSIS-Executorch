/*---------------------------------------------------------------------------
 * Bunny companion for the Alif DevKit-E8 (BOARD_BUNNY_DEMO).
 *
 * The CPU runs the behaviour: motion clips (bunny_clips.c, a port of
 * model/bunny/clips.py), a small state machine (idle, wander, sleep, reactions)
 * and blending/smoothing of the 30 pose dials. Every frame the Ethos-U85
 * draws the bunny from those dials (BunnyDecoder, nn_forward in app_main.cpp).
 * The CPU scales the sprite up, places and flips it, and draws the effects
 * (zzz, sweat, hearts), the touch dot and a small mic graph on a white screen.
 *
 *   touch the bunny      pet: love + hearts       touch elsewhere  run there
 *   hold on the bunny    hatch a new-looking bunny (identity dials)
 *   shake the board      startled shake           loud sound       ears perk up
 *   no input ~20 s       lie down, sleep; any input wakes it
 *   joystick A/B/C/D     wave / yawn / hop / clap; CENTER eat
 *---------------------------------------------------------------------------*/

#ifdef BOARD_BUNNY_DEMO

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "board_config.h"
#include "Driver_IO.h"
#include "Driver_I2C.h"
#include "Driver_Touch_Screen.h"

#include "display.h"
#include "camera.h"
#include "mic.h"
#include "imu.h"
#include "bunny_clips.h"
#include "se_services_port.h"
#include "Driver_WDT.h"

extern ARM_DRIVER_WDT Driver_WDT0;
extern uint32_t Image$$ARM_LIB_STACK$$ZI$$Base[];
extern uint32_t Image$$ARM_LIB_STACK$$ZI$$Limit[];
#define WATCHDOG_MS     3000    /* loop stuck this long -> the chip resets itself */
#define STACK_PAINT     0xA5A5A5A5u

/* Fill the unused stack with a pattern; stack_used() then finds the deepest use */
static void stack_paint(void)
{
    uint32_t *p = Image$$ARM_LIB_STACK$$ZI$$Base, *sp = (uint32_t *)__get_MSP();
    while (p < sp - 64) *p++ = STACK_PAINT;
}

static uint32_t stack_used(void)
{
    uint32_t *base = Image$$ARM_LIB_STACK$$ZI$$Base, *p = base;
    while (*p == STACK_PAINT) ++p;
    return (uint32_t)((uint8_t *)Image$$ARM_LIB_STACK$$ZI$$Limit - (uint8_t *)p);
}

extern int nn_forward(const float *in, int n_in, const float **out, int *n_out);

/* ---- tuning ------------------------------------------------------------ */

#define SLEEP_AFTER_S   20.0f   /* no input this long -> lie down and sleep   */
#define BLEND_S         0.45f   /* cross-fade between clips                   */
#define DIAL_SMOOTH     0.55f   /* per-frame low-pass on the dials (1 = none) */
#define WALK_PX_S       70.0f
#define RUN_PX_S        170.0f
#define RUN_IF_FARTHER  170.0f  /* px: run instead of walk                    */
#define HOLD_HATCH_S    1.2f    /* touch-and-hold on the bunny -> new bunny   */
#define IMU_WAKE_TH     25.0f   /* IMU motion that wakes him (still ~1-12)    */
#define IMU_SHAKE_TH    150.0f  /* IMU motion that counts as a shake          */
#define LOUD_DB         30.0f   /* mic level (dB of RMS) that makes ears perk */
#define SPRITE_PX       448     /* sprite size on screen (64 -> 448, 48 -> 432) */
#define EDGE_FRAC       0.22f   /* how far the (mostly empty) sprite square may leave the screen */
#define RELEASE_S       0.15f   /* no finger this long = lifted (GT911 sends gaps) */
#define COLD_START_MS   1000    /* let the panel power up before the display init */
#define GROUND_Y        690     /* sprite bottom on screen                    */
#define GRAPH_Y0        700     /* mic graph strip                            */

volatile uint32_t g_bunny_nn_us, g_bunny_fps;  /* debugger-readable */

#define SLOW_FRAME_MS   150     /* a frame slower than this prints its stage times */
#define IMU_EVERY       3       /* read the IMU every Nth frame                     */
#define IMU_SLOW_US     5000    /* an IMU read slower than this counts as a failure */
#define SENSOR_FAILS    3       /* consecutive failures before backing off          */

static inline uint32_t us_since(uint32_t c0)
{
    return (DWT->CYCCNT - c0) / (SystemCoreClock / 1000000u);
}

/* ---- small helpers ----------------------------------------------------- */

static uint32_t rng_state = 2463534242u;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rng() >> 8) * (1.0f / 16777216.0f);
}

static float smoothstep(float t)
{
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static inline void write_px(uint8_t *p, int bpp, uint8_t r, uint8_t g, uint8_t b)
{
    if (bpp >= 3) {
        p[0] = b;  /* the panel reads the bytes as blue, green, red */
        p[1] = g;
        p[2] = r;
        if (bpp == 4) p[3] = 255;
    } else {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)(v >> 8);
    }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *fb = display_buffer();
    int w = display_width(), h = display_height(), bpp = display_bytes_per_pixel();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; ++y) {
        uint8_t *p = fb + ((size_t)y * (size_t)w + (size_t)x0) * (size_t)bpp;
        for (int x = x0; x < x1; ++x, p += bpp) write_px(p, bpp, r, g, b);
    }
}

static void fill_circle(int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = -rad; y <= rad; ++y) {
        int half = (int)sqrtf((float)(rad * rad - y * y));
        fill_rect(cx - half, cy + y, cx + half + 1, cy + y + 1, r, g, b);
    }
}

/* Thick line by stamping squares */
static void thick_line(int x0, int y0, int x1, int y1, int t, uint8_t r, uint8_t g, uint8_t b)
{
    int n = (int)(fabsf((float)(x1 - x0)) > fabsf((float)(y1 - y0)) ? fabsf((float)(x1 - x0)) : fabsf((float)(y1 - y0)));
    if (n < 1) n = 1;
    for (int i = 0; i <= n; ++i) {
        int x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
        fill_rect(x - t / 2, y - t / 2, x + (t + 1) / 2, y + (t + 1) / 2, r, g, b);
    }
}

/* Filled heart, size s px (implicit heart curve) */
static void fill_heart(int cx, int cy, int s, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = -s; y <= s; ++y) {
        for (int x = -s; x <= s; ++x) {
            float fx = (float)x / (float)s * 1.2f, fy = -(float)y / (float)s * 1.2f + 0.25f;
            float a = fx * fx + fy * fy - 1.0f;
            if (a * a * a - fx * fx * fy * fy * fy <= 0.0f) fill_rect(cx + x, cy + y, cx + x + 1, cy + y + 1, r, g, b);
        }
    }
}

/* ---- touch (GT911 on the shared I2C1; polled, see HANDOFF) ------------- */

extern ARM_DRIVER_TOUCH_SCREEN GT911;
extern ARM_DRIVER_I2C Driver_I2C1;
static uint8_t gt_addr = 0x14;

static int gt_wait(void)
{
    uint32_t t0 = DWT->CYCCNT, limit = SystemCoreClock / 50U;  /* 20 ms */
    while (Driver_I2C1.GetStatus().busy) {
        if (DWT->CYCCNT - t0 > limit) return -1;
    }
    return 0;
}

static int gt_write(uint16_t reg, const uint8_t *data, int n)
{
    uint8_t buf[8];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)reg;
    for (int i = 0; i < n; ++i) buf[2 + i] = data[i];
    if (Driver_I2C1.MasterTransmit(gt_addr, buf, (uint32_t)(2 + n), false) != ARM_DRIVER_OK) return -1;
    return gt_wait();
}

static int gt_read(uint16_t reg, uint8_t *out, int n)
{
    if (gt_write(reg, NULL, 0) != 0) return -1;
    if (Driver_I2C1.MasterReceive(gt_addr, out, (uint32_t)n, false) != ARM_DRIVER_OK) return -1;
    return gt_wait();
}

static int gt_setup(void)
{
    GT911.Initialize();
    GT911.PowerControl(ARM_POWER_FULL);  /* resets the chip; its own I2C check fails, expected */
    static const uint8_t addrs[2] = {0x14, 0x5D};
    for (int i = 0; i < 2; ++i) {
        uint8_t zero = 0, id[4] = {0};
        gt_addr = addrs[i];
        gt_write(0x8040, &zero, 1);
        if (gt_read(0x8140, id, 4) == 0 && id[0] == '9') return 0;
    }
    return -1;
}

/* 1 = new touch report with a finger down at (*x, *y), 0 = no new report,
 * 2 = new report with no finger (released), -1 = error */
static int gt_poll(int *x, int *y)
{
    uint8_t st = 0, p[4], zero = 0;
    if (gt_read(0x814E, &st, 1) != 0) return -1;
    if (!(st & 0x80)) return 0;
    int n = st & 0x0F, got = 2;
    if (n > 0 && gt_read(0x8150, p, 4) == 0) {
        *x = p[0] | (p[1] << 8);
        *y = p[2] | (p[3] << 8);
        got = 1;
    }
    gt_write(0x814E, &zero, 1);
    return got;
}

/* ---- LEDs and joystick ------------------------------------------------- */

extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_B_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB1_R_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB1_G_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB1_B_GPIO_PORT);
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(15); /* joystick */

static ARM_DRIVER_GPIO *const led_drv[6] = {
    &ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT), &ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT),
    &ARM_Driver_GPIO_(BOARD_LEDRGB0_B_GPIO_PORT), &ARM_Driver_GPIO_(BOARD_LEDRGB1_R_GPIO_PORT),
    &ARM_Driver_GPIO_(BOARD_LEDRGB1_G_GPIO_PORT), &ARM_Driver_GPIO_(BOARD_LEDRGB1_B_GPIO_PORT),
};
static const uint8_t led_pin_no[6] = {
    BOARD_LEDRGB0_R_GPIO_PIN, BOARD_LEDRGB0_G_GPIO_PIN, BOARD_LEDRGB0_B_GPIO_PIN,
    BOARD_LEDRGB1_R_GPIO_PIN, BOARD_LEDRGB1_G_GPIO_PIN, BOARD_LEDRGB1_B_GPIO_PIN,
};
static const uint8_t joy_pin[5] = {
    BOARD_JOY_SW_A_GPIO_PIN, BOARD_JOY_SW_B_GPIO_PIN, BOARD_JOY_SW_C_GPIO_PIN,
    BOARD_JOY_SW_D_GPIO_PIN, BOARD_JOY_SW_CENTER_GPIO_PIN,
};

static int led_last = -1;

static void leds_init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < 6; ++i) {
        led_drv[i]->Initialize(led_pin_no[i], NULL);
        led_drv[i]->PowerControl(led_pin_no[i], ARM_POWER_FULL);
        led_drv[i]->SetDirection(led_pin_no[i], GPIO_PIN_DIRECTION_OUTPUT);
        led_drv[i]->SetValue(led_pin_no[i], GPIO_PIN_OUTPUT_STATE_LOW);
    }
}

static void leds_rgb(int r, int g, int b)
{
    int want = (r ? 4 : 0) | (g ? 2 : 0) | (b ? 1 : 0);
    if (want == led_last) return;
    led_last = want;
    const int on[6] = {r, g, b, r, g, b};
    for (int i = 0; i < 6; ++i) {
        led_drv[i]->SetValue(led_pin_no[i], on[i] ? GPIO_PIN_OUTPUT_STATE_HIGH : GPIO_PIN_OUTPUT_STATE_LOW);
    }
}

/* Boot progress (called from main.c too): red = started, yellow = clocks,
 * blue = model loaded / display starting, green = running, blinking red =
 * display failed. */
void bunny_boot_led(int r, int g, int b)
{
    leds_init_once();
    leds_rgb(r, g, b);
}

static void busy_ms(uint32_t ms)
{
    uint32_t t0 = DWT->CYCCNT, n = (SystemCoreClock / 1000U) * ms;
    while (DWT->CYCCNT - t0 < n) {}
}

static void io_init(void)
{
    leds_init_once();
    for (int j = 0; j < 5; ++j) {
        ARM_Driver_GPIO_(15).Initialize(joy_pin[j], NULL);
        ARM_Driver_GPIO_(15).PowerControl(joy_pin[j], ARM_POWER_FULL);
        ARM_Driver_GPIO_(15).SetDirection(joy_pin[j], GPIO_PIN_DIRECTION_INPUT);
    }
}

/* ---- crash reporter ---------------------------------------------------- */

/* A fault prints where it happened (UART) and blinks the LEDs purple instead
 * of freezing silently. The stacked PC/LR map to source lines with addr2line. */
void bunny_fault_report(uint32_t *sp, uint32_t exc_return)
{
    printf("FAULT pc=%08lx lr=%08lx cfsr=%08lx hfsr=%08lx bfar=%08lx mmfar=%08lx exc=%08lx\n",
           (unsigned long)sp[6], (unsigned long)sp[5], (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
           (unsigned long)SCB->BFAR, (unsigned long)SCB->MMFAR, (unsigned long)exc_return);
    for (;;) {
        for (volatile uint32_t i = 0; i < 8000000u; ++i) {}
        for (int k = 0; k < 6; ++k) led_drv[k]->SetValue(led_pin_no[k], (k % 3 != 1) ? GPIO_PIN_OUTPUT_STATE_HIGH : GPIO_PIN_OUTPUT_STATE_LOW);
        for (volatile uint32_t i = 0; i < 8000000u; ++i) {}
        for (int k = 0; k < 6; ++k) led_drv[k]->SetValue(led_pin_no[k], GPIO_PIN_OUTPUT_STATE_LOW);
    }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4 \n"
        "ite eq \n"
        "mrseq r0, msp \n"
        "mrsne r0, psp \n"
        "mov r1, lr \n"
        "b bunny_fault_report \n");
}

/* ---- sprite: NN output -> screen --------------------------------------- */

#define MAX_NN  96
#define MAX_SPR 480

static uint8_t  src_rgb[MAX_NN * MAX_NN * 3];
static uint16_t tab_i0[MAX_SPR];
static uint16_t tab_w[MAX_SPR];  /* weight of the second sample, 0..256 */
static int      nn_side, spr;

static void build_tables(void)
{
    for (int d = 0; d < spr; ++d) {
        int pos256 = ((2 * d + 1) * 256 * nn_side) / (2 * spr) - 128;  /* (d+0.5)*S/spr - 0.5 */
        if (pos256 < 0) pos256 = 0;
        if (pos256 > (nn_side - 1) * 256) pos256 = (nn_side - 1) * 256;
        int i0 = pos256 >> 8;
        if (i0 > nn_side - 2) i0 = nn_side - 2;
        tab_i0[d] = (uint16_t)i0;
        tab_w[d] = (uint16_t)(pos256 - (i0 << 8));
    }
}

/* NN output (3 x S x S floats, 0..1) -> src_rgb */
static void convert_output(const float *out)
{
    int n = nn_side * nn_side;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = out[c * n + i] * 255.0f + 0.5f;
            src_rgb[i * 3 + c] = (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
        }
    }
}

/* Bilinear scale src_rgb to spr x spr at (ox, oy), mirrored if flip */
static void draw_sprite(int ox, int oy, int flip)
{
    uint8_t *fb = display_buffer();
    int w = display_width(), h = display_height(), bpp = display_bytes_per_pixel();
    static uint8_t vrow[MAX_NN * 3];
    /* visible x range of the sprite */
    int x_lo = ox < 0 ? -ox : 0, x_hi = ox + spr > w ? w - ox : spr;
    for (int y = 0; y < spr; ++y) {
        int sy = oy + y;
        if (sy < 0 || sy >= h) continue;
        /* vertical blend once per row, then only the horizontal blend per pixel */
        const uint8_t *r0 = src_rgb + (size_t)tab_i0[y] * nn_side * 3;
        const uint8_t *r1 = r0 + nn_side * 3;
        uint32_t wy = tab_w[y], wy0 = 256 - wy;
        for (int k = 0; k < nn_side * 3; ++k) {
            vrow[k] = (uint8_t)((r0[k] * wy0 + r1[k] * wy) >> 8);
        }
        uint8_t *row = fb + (size_t)sy * (size_t)w * (size_t)bpp;
        for (int x = x_lo; x < x_hi; ++x) {
            int src_x = flip ? spr - 1 - x : x;
            const uint8_t *v = vrow + tab_i0[src_x] * 3;
            uint32_t wx = tab_w[src_x], wx0 = 256 - wx;
            uint8_t r = (uint8_t)((v[0] * wx0 + v[3] * wx) >> 8);
            uint8_t g = (uint8_t)((v[1] * wx0 + v[4] * wx) >> 8);
            uint8_t b = (uint8_t)((v[2] * wx0 + v[5] * wx) >> 8);
            write_px(row + (size_t)(ox + x) * (size_t)bpp, bpp, r, g, b);
        }
    }
}

typedef struct { int x0, y0, x1, y1; } rect_t;

/* Clear (to white) the parts of old not covered by cur */
static void clear_uncovered(rect_t old, rect_t cur)
{
    if (old.x1 <= old.x0) return;
    if (cur.x1 <= old.x0 || cur.x0 >= old.x1 || cur.y1 <= old.y0 || cur.y0 >= old.y1) {
        fill_rect(old.x0, old.y0, old.x1, old.y1, 255, 255, 255);
        return;
    }
    if (old.y0 < cur.y0) fill_rect(old.x0, old.y0, old.x1, cur.y0, 255, 255, 255);
    if (old.y1 > cur.y1) fill_rect(old.x0, cur.y1, old.x1, old.y1, 255, 255, 255);
    int ya = old.y0 > cur.y0 ? old.y0 : cur.y0, yb = old.y1 < cur.y1 ? old.y1 : cur.y1;
    if (old.x0 < cur.x0) fill_rect(old.x0, ya, cur.x0, yb, 255, 255, 255);
    if (old.x1 > cur.x1) fill_rect(cur.x1, ya, old.x1, yb, 255, 255, 255);
}

/* Effects at rig coordinates (512 canvas, see bunny_rig3.bunny_svg) */
static void draw_effects(const bunny_pose_t *p, int ox, int oy, int flip)
{
    float k = (float)spr / 512.0f;
#define FX(X) (ox + (int)((flip ? 512.0f - (X) : (X)) * k))
#define FY(Y) (oy + (int)((Y) * k))
    if (p->zzz > 0.3f) {
        float lie = p->d[D_LIE];
        float sx = -60.0f * lie, sy = 120.0f * lie;
        int t = spr / 60 + 2;
        struct { float x, y, s; } z[3] = {{378, 80, 40}, {420, 50, 28}, {448, 26, 20}};
        for (int i = 0; i < 3; ++i) {
            float x0 = z[i].x + sx, y0 = z[i].y + sy, s = z[i].s;
            thick_line(FX(x0), FY(y0), FX(x0 + s), FY(y0), t, 70, 40, 95);
            thick_line(FX(x0 + s), FY(y0), FX(x0), FY(y0 + s), t, 70, 40, 95);
            thick_line(FX(x0), FY(y0 + s), FX(x0 + s), FY(y0 + s), t, 70, 40, 95);
        }
    }
    if (p->sweat > 0.3f) {
        fill_circle(FX(392), FY(200), (int)(14 * k) + 1, 159, 216, 255);
        thick_line(FX(392), FY(172), FX(392), FY(196), (int)(10 * k) + 1, 159, 216, 255);
    }
    if (p->hearts > 0.3f) {
        float s = p->hearts;
        fill_heart(FX(400), FY(120), (int)(26 * k * s) + 2, 240, 100, 143);
        fill_heart(FX(120), FY(90), (int)(20 * k * s) + 2, 240, 100, 143);
        fill_heart(FX(430), FY(190), (int)(16 * k * s) + 2, 240, 100, 143);
    }
#undef FX
#undef FY
}

/* ---- mic graph --------------------------------------------------------- */

#define GRAPH_N 80
static float graph_db[GRAPH_N];

static void draw_graph(int w, int h)
{
    fill_rect(0, GRAPH_Y0, w, h, 244, 240, 250);
    int bw = w / GRAPH_N, gh = h - GRAPH_Y0 - 8;
    for (int i = 0; i < GRAPH_N; ++i) {
        float t = graph_db[i] / 40.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        int bh = (int)(t * (float)gh);
        fill_rect(i * bw + 1, h - 4 - bh, i * bw + bw - 1, h - 4, 160, 120, 230);
    }
}

/* ---- behaviour --------------------------------------------------------- */

typedef enum { ST_IDLE, ST_WANDER, ST_ACTION, ST_LIE, ST_SLEEP } state_t;

static const char *const state_names[] = {"idle", "wander", "action", "lie", "sleep"};

static bunny_clip_t clip = CLIP_IDLE;
static float        clip_t;      /* time in the current clip */
static float        blend_t = BLEND_S;
static float        from_d[BUNNY_NDIAL], from_dx, from_dy;
static bunny_pose_t shown;       /* smoothed pose that is drawn */
static float        identity[4] = {0.0f, 1.0f, 128.0f, 1.0f};  /* hue, sat, ear_len, head_scale */

static void set_clip(bunny_clip_t c)
{
    if (c == clip) return;
    memcpy(from_d, shown.d, sizeof(from_d));
    from_dx = shown.dx;
    from_dy = shown.dy;
    clip = c;
    clip_t = 0.0f;
    blend_t = 0.0f;
}

static void hatch(void)
{
    static const int ids[4] = {D_HUE, D_SAT, D_EAR_LEN, D_HEAD_SCALE};
    for (int i = 0; i < 4; ++i) {
        float lo, hi;
        bunny_dial_range(ids[i], &lo, &hi);
        identity[i] = frand(lo, hi);
    }
    printf("bunny: hatched hue=%d sat=%d ear=%d head=%d\n", (int)(identity[0] * 1000), (int)(identity[1] * 100),
           (int)identity[2], (int)(identity[3] * 100));
}

/* ---- main loop --------------------------------------------------------- */

void bunny_demo_loop(void)
{
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    bunny_boot_led(0, 0, 1);  /* blue: model loaded, starting the display */
    /* After a USB power-on the display init hung (a RESET always worked):
     * give the panel and its supplies time to come up first. */
    busy_ms(COLD_START_MS);

    /* The LCD's DPHY needs the SE's 20 MHz clock; after some power cycles the
     * first request fails, so re-request it and retry the display a few times. */
    int display_rc = -1;
    for (int attempt = 0; attempt < 5 && display_rc != 0; ++attempt) {
        if (attempt > 0) {
            uint32_t err = 0;
            SERVICES_clocks_enable_clock(se_services_s_handle, CLKEN_CLK_20M, true, &err);
            busy_ms(300);
        }
        display_rc = display_init();
        printf("bunny: display_init attempt %d -> %d\n", attempt + 1, display_rc);
    }
    /* The camera opens I2C1 first so its callback owns the bus (touch polls) */
    int camera_rc = camera_init();
    int mic_rc = mic_init();
    io_init();
    int touch_rc = gt_setup();
    int imu_rc = imu_init();
    printf("bunny: display=%d camera=%d mic=%d touch=%d imu=%d\n", display_rc, camera_rc, mic_rc, touch_rc, imu_rc);
    if (display_rc != 0) {
        printf("bunny: no display, stopping\n");
        for (;;) {  /* blinking red: display failed */
            bunny_boot_led(1, 0, 0);
            busy_ms(150);
            bunny_boot_led(0, 0, 0);
            busy_ms(150);
        }
    }
    bunny_boot_led(0, 1, 0);  /* green: display up */

    int w = display_width(), h = display_height();
    fill_rect(0, 0, w, h, 255, 255, 255);

    /* First run tells the output size (3 x S x S) */
    float nn_in[BUNNY_NDIAL];
    const float *out = NULL;
    int n_out = 0;
    bunny_clip(CLIP_IDLE, 0.0f, 1.0f, &shown);
    bunny_normalize(shown.d, nn_in);
    if (nn_forward(nn_in, BUNNY_NDIAL, &out, &n_out) != 0) {
        printf("bunny: model is not the BunnyDecoder (nn_forward failed)\n");
        for (;;) {}
    }
    nn_side = (int)(sqrtf((float)n_out / 3.0f) + 0.5f);
    if (nn_side > MAX_NN || nn_side < 2) {
        printf("bunny: unexpected output size %d\n", n_out);
        for (;;) {}
    }
    spr = (SPRITE_PX / nn_side) * nn_side;
    if (spr > MAX_SPR) spr = MAX_SPR;
    build_tables();
    printf("bunny: NN draws %dx%d, shown at %dx%d\n", nn_side, nn_side, spr, spr);

    state_t st = ST_IDLE;
    /* The bunny fills only the middle of its square, so let the square go
     * partly off-screen: the whole width stays usable for wandering. */
    float x_min = -EDGE_FRAC * (float)spr, x_max = (float)(w - spr) + EDGE_FRAC * (float)spr;
    float bx = (float)(w - spr) / 2.0f, target_x = bx;
    int facing_left = 0;
    float state_t_s = 0.0f, next_decision = 3.0f, idle_s = 0.0f, action_len = 0.0f;
    float perk = 0.0f, hold_s = 0.0f;
    int touching = 0, touch_x = 0, touch_y = 0, hatched_this_hold = 0;
    float since_finger = 1.0f;  /* s since the last finger-down report */
    float touch_pause = 0.0f, imu_last = 0.0f;
    int touch_fails = 0, imu_fails = 0;
    uint32_t was_pressed = 0, frame = 0, nn_us_acc = 0;
    rect_t prev = {0, 0, 0, 0}, prev_dot = {0, 0, 0, 0};
    uint32_t t_prev = DWT->CYCCNT, t_stat = t_prev;

    stack_paint();
    int wdt_rc = Driver_WDT0.Initialize(WATCHDOG_MS);
    if (wdt_rc == ARM_DRIVER_OK) wdt_rc = Driver_WDT0.PowerControl(ARM_POWER_FULL);
    if (wdt_rc == ARM_DRIVER_OK) wdt_rc = Driver_WDT0.Control(ARM_WATCHDOG_UNLOCK, 0);
    if (wdt_rc == ARM_DRIVER_OK) wdt_rc = Driver_WDT0.Start();
    printf("bunny: watchdog %d ms -> %d\n", WATCHDOG_MS, wdt_rc);
    printf("bunny: running\n");
    for (;;) {
        uint32_t now = DWT->CYCCNT;
        float dt = (float)(now - t_prev) / (float)SystemCoreClock;
        t_prev = now;
        if (dt > 0.1f) dt = 0.1f;
        rng_state ^= now;

        /* ---- inputs ---- */
        int any_input = 0, poked = 0;
        uint32_t st0 = DWT->CYCCNT, us_touch = 0, us_imu = 0, us_mic = 0, us_nn = 0, us_draw = 0;
        if (touch_pause > 0.0f) touch_pause -= dt;
        if (touch_rc == 0 && touch_pause <= 0.0f) {
            int tx, ty, r = gt_poll(&tx, &ty);
            if (r < 0) {
                if (++touch_fails >= SENSOR_FAILS) {  /* bus trouble: pause touch for 1 s */
                    printf("touch: %d failed reads, pausing 1 s\n", touch_fails);
                    touch_pause = 1.0f;
                    touch_fails = 0;
                }
            } else {
                touch_fails = 0;
            }
            if (r == 1 && (tx >= w || ty >= h)) {
                r = 0;  /* ignore coordinates off the panel */
            }
            since_finger += dt;
            if (r == 1) {
                since_finger = 0.0f;
                if (!touching) {
                    poked = 1;
                    printf("touch: down x=%d y=%d frame %lu\n", tx, ty, (unsigned long)frame);
                }
                touching = 1;
                touch_x = tx;
                touch_y = ty;
                any_input = 1;
            } else if (touching && since_finger > RELEASE_S) {
                touching = 0;  /* finger lifted */
            }
        }
        /* hit box: the middle of the sprite square, where the bunny is drawn */
        int on_bunny = touching && touch_x >= (int)bx + spr / 5 && touch_x < (int)bx + spr * 4 / 5 &&
                       touch_y >= GROUND_Y - spr * 17 / 20 && touch_y < GROUND_Y;

        us_touch = us_since(st0);
        uint32_t st1 = DWT->CYCCNT;
        float motion = 0.0f;
        if (imu_rc == 0 && frame % IMU_EVERY == 0) {
            imu_poll(&imu_last, NULL, NULL, NULL);
            us_imu = us_since(st1);
            if (us_imu > IMU_SLOW_US) {
                if (++imu_fails >= SENSOR_FAILS) {  /* the I3C bus keeps timing out: stop using it */
                    printf("imu: %d slow reads (%lu us), IMU off\n", imu_fails, (unsigned long)us_imu);
                    imu_rc = -9;
                }
            } else {
                imu_fails = 0;
            }
        }
        motion = imu_last;
        uint32_t st2 = DWT->CYCCNT;
        float level = 0.0f;
        int loud = 0;
        int mic_blocks_now = 0;
        while (mic_rc == 0 && mic_blocks_now++ < 8 && mic_poll(&level) == 1) {
            float db = level > 1.0f ? 20.0f * log10f(level) : 0.0f;
            memmove(graph_db, graph_db + 1, sizeof(graph_db) - sizeof(graph_db[0]));
            graph_db[GRAPH_N - 1] = db;
            if (db > LOUD_DB) loud = 1;
        }

        uint32_t pressed = 0;
        for (int j = 0; j < 5; ++j) {
            uint32_t v = 1;
            ARM_Driver_GPIO_(15).GetValue(joy_pin[j], &v);
            if (v == 0U) pressed |= 1u << j;
        }
        uint32_t newly = pressed & ~was_pressed;
        was_pressed = pressed;
        if (newly) any_input = 1;
        if (motion > IMU_WAKE_TH || loud) any_input = 1;

        /* ---- behaviour ---- */
        state_t_s += dt;
        idle_s = any_input ? 0.0f : idle_s + dt;

        if (st == ST_LIE || st == ST_SLEEP) {
            if (any_input) {  /* bolt awake */
                st = ST_ACTION;
                set_clip(motion > IMU_SHAKE_TH ? CLIP_SHAKE : CLIP_STRETCH);
                action_len = bunny_clip_duration(clip);
                state_t_s = 0.0f;
            } else if (st == ST_LIE && state_t_s > bunny_clip_duration(CLIP_LIE_DOWN)) {
                st = ST_SLEEP;
                set_clip(CLIP_SLEEP);
                blend_t = BLEND_S;  /* lie_down ends where sleep starts */
            }
        } else {
            bunny_clip_t act = CLIP_COUNT;
            if (motion > IMU_SHAKE_TH) act = CLIP_SHAKE;
            else if (newly & 1u) act = CLIP_WAVE;
            else if (newly & 2u) act = CLIP_YAWN;
            else if (newly & 4u) act = CLIP_HOP;
            else if (newly & 8u) act = CLIP_CLAP;
            else if (newly & 16u) act = CLIP_EAT;
            else if (poked && on_bunny) act = CLIP_LOVE;

            if (touching && on_bunny) {
                hold_s += dt;
                if (hold_s > HOLD_HATCH_S && !hatched_this_hold) {
                    hatch();
                    hatched_this_hold = 1;
                    act = CLIP_HOP;
                }
            } else {
                hold_s = 0.0f;
                hatched_this_hold = 0;
            }

            if (act != CLIP_COUNT && !(act == clip && st == ST_ACTION)) {
                st = ST_ACTION;
                set_clip(act);
                action_len = bunny_clip_duration(act) * (act == CLIP_HOP || act == CLIP_LOVE ? 1.0f : 1.5f);
                state_t_s = 0.0f;
            } else if (poked && !on_bunny) {  /* run to where you touched */
                target_x = (float)touch_x - (float)spr / 2.0f;
                st = ST_WANDER;
                state_t_s = 0.0f;
            }

            switch (st) {
            case ST_ACTION:
                if (state_t_s > action_len) {
                    st = ST_IDLE;
                    set_clip(CLIP_IDLE);
                    state_t_s = 0.0f;
                    next_decision = frand(2.0f, 5.0f);
                }
                break;
            case ST_WANDER: {
                float dist = target_x - bx;
                if (target_x < x_min) target_x = x_min;
                if (target_x > x_max) target_x = x_max;
                if (fabsf(dist) < 4.0f) {
                    st = ST_IDLE;
                    set_clip(CLIP_IDLE);
                    state_t_s = 0.0f;
                    next_decision = frand(2.0f, 5.0f);
                } else {
                    int fast = fabsf(dist) > RUN_IF_FARTHER;
                    set_clip(fast ? CLIP_RUN : CLIP_WALK);
                    float step = (fast ? RUN_PX_S : WALK_PX_S) * dt;
                    facing_left = dist < 0.0f;
                    bx += dist > 0.0f ? (step < dist ? step : dist) : (-step > dist ? -step : dist);
                }
                break;
            }
            case ST_IDLE:
            default:
                if (idle_s > SLEEP_AFTER_S) {
                    st = ST_LIE;
                    set_clip(CLIP_LIE_DOWN);
                    state_t_s = 0.0f;
                } else if (state_t_s > next_decision) {
                    float r = frand(0.0f, 1.0f);
                    state_t_s = 0.0f;
                    next_decision = frand(3.0f, 6.0f);
                    if (r < 0.55f) {
                        target_x = frand(x_min, x_max);
                        st = ST_WANDER;
                    } else if (r < 0.8f) {
                        set_clip(CLIP_LOOK);
                    } else {
                        set_clip(r < 0.9f ? CLIP_SIT : CLIP_IDLE);
                    }
                }
                break;
            }
        }
        perk = loud ? 1.2f : (perk > dt ? perk - dt : 0.0f);

        /* ---- pose: clip, cross-fade, identity, perk, smoothing ---- */
        clip_t += dt;
        blend_t += dt;
        bunny_pose_t target;
        bunny_clip(clip, clip_t, 1.0f, &target);
        float u = smoothstep(blend_t / BLEND_S);
        for (int i = 0; i < BUNNY_NDIAL; ++i) target.d[i] = from_d[i] + (target.d[i] - from_d[i]) * u;
        target.dx = from_dx + (target.dx - from_dx) * u;
        target.dy = from_dy + (target.dy - from_dy) * u;
        target.d[D_HUE] = identity[0];
        target.d[D_SAT] = identity[1];
        target.d[D_EAR_LEN] = identity[2];
        target.d[D_HEAD_SCALE] = identity[3];
        if (perk > 0.0f && st != ST_SLEEP) {
            float p = perk > 1.0f ? 1.0f : perk;
            target.d[D_EAR_L] += (-2.0f - target.d[D_EAR_L]) * p;
            target.d[D_EAR_R] += (2.0f - target.d[D_EAR_R]) * p;
            target.d[D_CURL] *= 1.0f - p;
            for (int i = D_EYE_OPEN; i <= D_EYE_SAD; ++i) target.d[i] *= 1.0f - 0.8f * p;
            target.d[D_EYE_WIDE] += 0.8f * p;
            target.d[D_LOOK_Y] = -3.0f * p;
        }
        if (frame == 0) shown = target;
        for (int i = 0; i < BUNNY_NDIAL; ++i) shown.d[i] += (target.d[i] - shown.d[i]) * DIAL_SMOOTH;
        shown.dx += (target.dx - shown.dx) * DIAL_SMOOTH;
        shown.dy += (target.dy - shown.dy) * DIAL_SMOOTH;
        shown.zzz = target.zzz;
        shown.sweat = target.sweat;
        shown.hearts = target.hearts;

        /* ---- NN: the Ethos-U85 draws the bunny ---- */
        bunny_normalize(shown.d, nn_in);
        us_mic = us_since(st2);
        uint32_t c0 = DWT->CYCCNT;
        int rc = nn_forward(nn_in, BUNNY_NDIAL, &out, &n_out);
        nn_us_acc += (DWT->CYCCNT - c0) / (SystemCoreClock / 1000000u);
        if (rc == 0) convert_output(out);

        us_nn = us_since(c0);
        uint32_t st3 = DWT->CYCCNT;
        /* ---- draw ---- */
        float k = (float)spr / 512.0f;
        int ox = (int)bx + (int)(shown.dx * k);
        int oy = GROUND_Y - spr + (int)(shown.dy * k);
        rect_t cur = {ox, oy, ox + spr, oy + spr};
        fill_rect(prev_dot.x0, prev_dot.y0, prev_dot.x1, prev_dot.y1, 255, 255, 255);
        clear_uncovered(prev, cur);
        draw_sprite(ox, oy, facing_left);
        draw_effects(&shown, ox, oy, facing_left);
        prev = cur;
        prev_dot = (rect_t){0, 0, 0, 0};
        if (touching && touch_y < GRAPH_Y0 - 8) {
            fill_circle(touch_x, touch_y, 6, 120, 80, 200);
            prev_dot = (rect_t){touch_x - 7, touch_y - 7, touch_x + 8, touch_y + 8};
        }
        draw_graph(w, h);
        display_flush();
        if (wdt_rc == ARM_DRIVER_OK) Driver_WDT0.Feed();
        us_draw = us_since(st3);
        if (us_since(st0) > SLOW_FRAME_MS * 1000u) {
            printf("slow frame %lu: touch %lu us, imu %lu us, mic %lu us, nn %lu us, draw %lu us\n",
                   (unsigned long)frame, (unsigned long)us_touch, (unsigned long)us_imu, (unsigned long)us_mic,
                   (unsigned long)us_nn, (unsigned long)us_draw);
        }

        /* LEDs follow the mood */
        if (clip == CLIP_LOVE) leds_rgb(1, 0, 1);
        else if (clip == CLIP_SHAKE) leds_rgb(1, 1, 0);
        else if (clip == CLIP_HOP || clip == CLIP_CLAP) leds_rgb(0, 1, 0);
        else if (perk > 0.0f) leds_rgb(0, 0, 1);
        else leds_rgb(0, ((now / (SystemCoreClock / 2U)) & 3U) == 0U, 0);  /* slow green heartbeat */

        if (++frame % 60 == 0) {
            uint32_t t1 = DWT->CYCCNT;
            g_bunny_fps = (uint32_t)(60ull * SystemCoreClock / (t1 - t_stat));
            g_bunny_nn_us = nn_us_acc / 60;
            t_stat = t1;
            nn_us_acc = 0;
            printf("bunny: %lu fps, NN %lu us, state %s, clip %s, imu %d, mic %d dB, stack %lu B\n",
                   (unsigned long)g_bunny_fps, (unsigned long)g_bunny_nn_us, state_names[st],
                   bunny_clip_name(clip), (int)motion, (int)graph_db[GRAPH_N - 1], (unsigned long)stack_used());
        }
    }
}

#endif /* BOARD_BUNNY_DEMO */
