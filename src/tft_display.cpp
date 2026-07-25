#include "tft_display.h"
#include "config.h"
#include "state_bus.h"
#include "device_motor.h"
#include "external_reference.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <string.h>

// Two screens, looping like tabs (safe/danger status screen removed by
// request - readings only, no verdict wording):
//   1. Name - "ZYGREEN" branding. Shown longer once at boot (splash), then
//      participates in the loop at the normal cadence.
//   2. Readings - CO2 (local MQ135) and PM2.5 (backend-resolved external
//      reference sensor) - display only, never a control input here.
// The two alternate forever, each change animated as a slide (old screen
// pushed off to the left as the new one enters from the right). The TFT
// never originates a command, only displays state.
static Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);

// Fixed panel geometry (1.8" ST7735, rotation(1) landscape) - used to size
// the offscreen canvases below, so these must match tft.setRotation(1)'s
// actual dimensions for this panel.
#define TFT_W 160
#define TFT_H 128

#define TFT_REFRESH_MS       600UL   // static-screen draw throttle - slow enough to read comfortably, not a flicker
#define LOGO_SPLASH_MS      6000UL   // first showing only - boot splash, solid (no blink - a blink was making it easy to miss entirely)
#define LOGO_LOOP_MS        4000UL   // every later showing, once it's just a tab in the loop
#define READINGS_DURATION_MS 4000UL  // "at least 4 sec"

// Slide transition, done incrementally across many tft_update() ticks (one
// step per call, never the whole animation in one call) - this is the same
// non-blocking discipline every other device_*.cpp state machine on this
// core follows: Core 1 must never stall waiting on anything, including its
// own SPI writes, since that would delay the mist/solenoid pulse timers
// running in the very same loop. Each step composes one full frame in RAM
// (fast, no SPI) then pushes it as a single SPI blit (~10-15ms at typical
// ST7735 SPI speeds for this panel's full 160x128 frame) - bounded and safe
// against the pulse timers' ~50ms tolerance, unlike doing the whole ~1.7s
// animation in one blocking call would have been.
#define TRANSITION_STEPS      24
#define TRANSITION_STEP_MS    70UL   // ~1.7s total - "slow"

enum TftScreen { SCR_LOGO, SCR_READINGS };
static TftScreen s_screen = SCR_LOGO;
static unsigned long s_screenEnteredAt = 0;
static unsigned long s_lastDraw = 0;
static bool s_bootSplashDone = false;   // logo's first showing is the longer boot splash; every later one is a normal tab

static bool s_transitioning = false;
static TftScreen s_transitionTo = SCR_READINGS;
static int s_transitionStep = 0;
static unsigned long s_lastStepAt = 0;

// Offscreen render targets for the two screens involved in a transition,
// plus one composite scratch buffer pushed to the real panel each step.
// Allocated once at startup and reused for every transition (never
// alloc/free per-transition) - this runs forever, and repeated large
// malloc/free cycles on an embedded heap is a real fragmentation risk over
// weeks of uptime.
static GFXcanvas16 s_canvasOld(TFT_W, TFT_H);
static GFXcanvas16 s_canvasNew(TFT_W, TFT_H);
static uint16_t s_frameBuf[TFT_W * TFT_H];

// ST77XX_ORANGE isn't defined in every Adafruit_ST7735 library version -
// computed via color565() instead, same approach the original esp32-control
// firmware used for exactly this reason.
static uint16_t ORANGE_565;

void tft_init() {
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  tft.initR(INITR_BLACKTAB);   // generic 1.8" ST7735 - try INITR_GREENTAB if colors look wrong
  tft.setRotation(1);
  tft.setTextWrap(false);
  ORANGE_565 = tft.color565(255, 140, 0);
  tft.fillScreen(ST77XX_BLACK);
  s_screen = SCR_LOGO;
  s_screenEnteredAt = millis();
  // The three GFXcanvas16/frameBuf buffers above reserve ~120KB of heap for
  // the life of the program - logged once here so a heap problem elsewhere
  // (e.g. during an OTA download, which needs its own TLS buffers
  // concurrently) shows up immediately in the serial log instead of as a
  // mystery crash.
  Serial.printf("[tft] free heap after init: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

// Generic draw functions - work identically whether `gfx` is the real
// panel (direct/static display) or an offscreen GFXcanvas16 (captured for
// a slide transition), since both are Adafruit_GFX.
static void drawLogoScreen(Adafruit_GFX &gfx) {
  gfx.fillScreen(ST77XX_BLACK);
  // The company name is the whole point of this screen - biggest text size
  // the 160px-wide panel can fit "ZYGREEN" at without clipping.
  gfx.setTextSize(3);
  gfx.setTextColor(ST77XX_GREEN);
  gfx.setCursor(17, 52);
  gfx.print("ZYGREEN");
}

static void drawReadingsScreen(Adafruit_GFX &gfx) {
  gfx.fillScreen(ST77XX_BLACK);

  gfx.setTextSize(1);
  gfx.setTextColor(ST77XX_WHITE);
  gfx.setCursor(10, 18);
  gfx.print("CO2");
  gfx.setTextSize(2);
  gfx.setTextColor(ST77XX_YELLOW);
  gfx.setCursor(10, 32);
  gfx.print(String(motor_get_last_mq135_ppm(), 0) + " ppm");

  // Backend-fetched reference value (the GOGREEN device's real sensor) -
  // display only. Shows "--" until the first reading arrives after
  // boot/reconnect.
  gfx.setTextSize(1);
  gfx.setTextColor(ST77XX_WHITE);
  gfx.setCursor(10, 68);
  gfx.print("PM2.5");
  gfx.setTextSize(2);
  gfx.setTextColor(ORANGE_565);
  gfx.setCursor(10, 82);
  if (external_reference_has_data()) {
    gfx.print(String(external_reference_get_pm25(), 1) + " ug/m3");
  } else {
    gfx.print("--");
  }
}

static void renderScreen(Adafruit_GFX &gfx, TftScreen screen) {
  switch (screen) {
    case SCR_LOGO:     drawLogoScreen(gfx); break;
    case SCR_READINGS: drawReadingsScreen(gfx); break;
  }
}

static TftScreen nextScreenAfter(TftScreen screen) {
  return screen == SCR_LOGO ? SCR_READINGS : SCR_LOGO;
}

static void startTransition(unsigned long now) {
  if (s_screen == SCR_LOGO) s_bootSplashDone = true;   // leaving the logo's first showing ends the splash
  TftScreen to = nextScreenAfter(s_screen);
  renderScreen(s_canvasOld, s_screen);   // capture current screen's content, in RAM only, no SPI
  renderScreen(s_canvasNew, to);         // capture the incoming screen the same way
  s_transitioning = true;
  s_transitionTo = to;
  s_transitionStep = 0;
  s_lastStepAt = now;
}

// One incremental slide step: old content pushed left, new content entering
// from the right - composed in RAM (fast), then a single SPI blit (bounded,
// see the comment on TRANSITION_STEPS above).
static void stepTransition() {
  int offset = (s_transitionStep * TFT_W) / TRANSITION_STEPS;   // 0..TFT_W
  const uint16_t *oldBuf = s_canvasOld.getBuffer();
  const uint16_t *newBuf = s_canvasNew.getBuffer();

  for (int row = 0; row < TFT_H; row++) {
    uint16_t *dst = s_frameBuf + row * TFT_W;
    if (offset < TFT_W) {
      memcpy(dst, oldBuf + row * TFT_W + offset, (TFT_W - offset) * sizeof(uint16_t));
    }
    if (offset > 0) {
      memcpy(dst + (TFT_W - offset), newBuf + row * TFT_W, offset * sizeof(uint16_t));
    }
  }
  tft.drawRGBBitmap(0, 0, s_frameBuf, TFT_W, TFT_H);
}

void tft_update() {
  unsigned long now = millis();

  if (s_transitioning) {
    if (now - s_lastStepAt < TRANSITION_STEP_MS) return;
    s_lastStepAt = now;
    stepTransition();
    s_transitionStep++;
    if (s_transitionStep > TRANSITION_STEPS) {
      s_transitioning = false;
      s_screen = s_transitionTo;
      s_screenEnteredAt = now;
    }
    return;
  }

  unsigned long elapsed = now - s_screenEnteredAt;
  unsigned long duration = s_screen == SCR_LOGO
                          ? (s_bootSplashDone ? LOGO_LOOP_MS : LOGO_SPLASH_MS)
                          : READINGS_DURATION_MS;
  if (elapsed >= duration) {
    startTransition(now);
    return;
  }

  if (now - s_lastDraw < TFT_REFRESH_MS) return;
  s_lastDraw = now;
  renderScreen(tft, s_screen);
}
