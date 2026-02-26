#include <Arduino.h>
#include <HID-Project.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// =====================
// CONFIG (ANPASSEN!)
// =====================
const uint8_t ROWS = 2;
const uint8_t COLS = 3;

// ROW->COL scan: Rows OUTPUT, Cols INPUT_PULLUP
// Du sagst: ENC-BTN ist Matrix (r=0,c=2) und hängt an A0 -> colPins[2] = A0
const uint8_t rowPins[ROWS] = {2, 3};
const uint8_t colPins[COLS] = {4, 5, 6};   // (0,2)=A0

// Encoder Pins: NICHT 14/15 nehmen, wenn A0/A1 schon Matrix sind (Kollision).
// Nimm echte Digitalpins passend zu deiner Verdrahtung:
const uint8_t ENC_A = 14;   // <--- anpassen
const uint8_t ENC_B = 15;   // <--- anpassen

// Encoder-Button sitzt IN DER MATRIX:
const uint8_t ENC_BTN_R = 0;
const uint8_t ENC_BTN_C = 2;

// Debounce (Matrix)
const uint16_t DEBOUNCE_MS = 25;

// HID an/aus
#define ENABLE_HID 1

// Layer
const uint8_t LAYERS = 6;

// Encoder Detent: oft 4 Steps pro Rastung.
// Wenn er noch springt: 8. Wenn zu träge: 2.
const int8_t ENC_STEPS_PER_DETENT = 4;

// RGB / NeoPixel
const uint8_t LED_PIN   = A3;     // <--- ggf. ändern
const uint8_t LED_COUNT = 6;      // 6 Layer => 6 LEDs
const uint8_t LED_BRIGHTNESS = 60; // 0..255
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// =====================
// RGB STATE MACHINE
// ENC-BTN cycled: Modus1 -> Modus2 -> AUS -> AN -> repeat
// =====================
enum RgbState : uint8_t {
  RGB_MODE1 = 0,   // Single-LED (Layer) + breathe
  RGB_MODE2 = 1,   // All-LEDs + breathe
  RGB_OFF   = 2,   // alles aus
  RGB_ON    = 3    // Single-LED (Layer) statisch (kein breathe)
};
RgbState rgbState = RGB_MODE1;

// =====================
// STATE
// =====================
bool stableState[ROWS][COLS] = {0};
bool lastReading[ROWS][COLS] = {0};
uint32_t lastChangeMs[ROWS][COLS] = {0};

uint8_t currentLayer = 0;
int8_t encAcc = 0;

// =====================
// KEYMAP
// =====================
#if ENABLE_HID
const KeyboardKeycode keymap[LAYERS][ROWS][COLS] = {
  // L0
  { { KEY_A, KEY_B, KEY_C }, { KEY_D, KEY_E, KEY_F } },
  // L1
  { { KEY_1, KEY_2, KEY_3 }, { KEY_4, KEY_5, KEY_6 } },
  // L2
  { { KEY_LEFT_ARROW, KEY_UP_ARROW,   KEY_RIGHT_ARROW },
    { KEY_DOWN_ARROW, KEY_ENTER,      KEY_ESC } },
  // L3
  { { KEY_F1, KEY_F2, KEY_F3 }, { KEY_F4, KEY_F5, KEY_F6 } },
  // L4
  { { KEY_HOME, KEY_PAGE_UP, KEY_END },
    { KEY_PAGE_DOWN, KEY_BACKSPACE, KEY_TAB } },
  // L5
  { { KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACE },
    { KEY_RIGHT_BRACE, KEY_BACKSLASH, KEY_SPACE } }
};
#endif

// =====================
// RGB HELPERS
// =====================
static inline uint32_t layerColor(uint8_t layer) {
  switch (layer % LAYERS) {
    case 0: return pixels.Color(  0, 180, 160); // türkis
    case 1: return pixels.Color(  0, 180,   0); // grün
    case 2: return pixels.Color(255, 110,   0); // orange
    case 3: return pixels.Color(160,   0, 255); // lila
    case 4: return pixels.Color(255,   0,  40); // pink/rot
    default:return pixels.Color( 40,  80, 255); // blau
  }
}

static inline uint8_t breatheFactor8(uint32_t now) {
  // 0..255, smooth breathing
  float t = (float)now / 380.0f;
  float s = (sinf(t) + 1.0f) * 0.5f;     // 0..1
  uint8_t v = (uint8_t)(30 + s * 225);   // min 30, max 255
  return v;
}

static inline uint32_t scaleColor(uint32_t c, uint8_t scale) {
  uint8_t r = (uint8_t)(c >> 16);
  uint8_t g = (uint8_t)(c >> 8);
  uint8_t b = (uint8_t)(c);
  r = (uint8_t)((uint16_t)r * scale / 255);
  g = (uint8_t)((uint16_t)g * scale / 255);
  b = (uint8_t)((uint16_t)b * scale / 255);
  return pixels.Color(r, g, b);
}

static void printRgbState() {
  Serial.print(F("RGB STATE -> "));
  switch (rgbState) {
    case RGB_MODE1: Serial.println(F("MODE1 (single+breathe)")); break;
    case RGB_MODE2: Serial.println(F("MODE2 (all+breathe)"));    break;
    case RGB_OFF:   Serial.println(F("OFF"));                   break;
    case RGB_ON:    Serial.println(F("ON (single static)"));    break;
  }
}

static inline void nextRgbState() {
  rgbState = (RgbState)((rgbState + 1) & 0x03);
  printRgbState();
}

static void rgbRender() {
  pixels.clear();

  if (rgbState == RGB_OFF) {
    pixels.show();
    return;
  }

  uint8_t br = 255;
  if (rgbState == RGB_MODE1 || rgbState == RGB_MODE2) {
    br = breatheFactor8(millis());
  }

  if (rgbState == RGB_MODE2) {
    for (uint8_t i = 0; i < LED_COUNT; i++) {
      pixels.setPixelColor(i, scaleColor(layerColor(i), br));
    }
  } else {
    uint8_t idx = currentLayer % LED_COUNT;
    pixels.setPixelColor(idx, scaleColor(layerColor(currentLayer), br));
  }

  pixels.show();
}

// =====================
// STARTUP ANIMATION
// =====================
static uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85)  return pixels.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return pixels.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170;    return pixels.Color(pos * 3, 255 - pos * 3, 0);
}

static void startupAnim() {
  // 1) schnell einmal durch
  pixels.clear();
  pixels.show();
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    pixels.clear();
    pixels.setPixelColor(i, pixels.Color(180, 180, 180));
    pixels.show();
    delay(30);
  }

  // 2) alle bunt + 1 puls
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    pixels.setPixelColor(i, wheel((i * 256 / LED_COUNT) & 255));
  }

  for (uint16_t k = 0; k < 170; k++) {
    float s = (sinf((float)k / 26.0f) + 1.0f) * 0.5f;
    uint8_t br = (uint8_t)(20 + s * 235);
    for (uint8_t i = 0; i < LED_COUNT; i++) {
      uint32_t col = wheel((i * 256 / LED_COUNT) & 255);
      pixels.setPixelColor(i, scaleColor(col, br));
    }
    pixels.show();
    delay(8);
  }
}

// =====================
// ENCODER (Gray table)
// =====================
static uint8_t encPrev = 0;
static const int8_t encTable[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

static inline uint8_t readEncAB() {
  uint8_t a = digitalRead(ENC_A) ? 1 : 0;
  uint8_t b = digitalRead(ENC_B) ? 1 : 0;
  return (a << 1) | b;
}

static void encoderInit() {
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  encPrev = readEncAB();
}

static int8_t encoderReadStep() {
  uint8_t curr = readEncAB();
  uint8_t idx  = (encPrev << 2) | curr;
  int8_t step  = encTable[idx];
  if (curr != encPrev) encPrev = curr;
  return step;
}

// =====================
// MATRIX SCAN
// =====================
static void selectRow(uint8_t r) {
  for (uint8_t i = 0; i < ROWS; i++) digitalWrite(rowPins[i], HIGH);
  digitalWrite(rowPins[r], LOW);
  delayMicroseconds(120);
}

static inline bool isEncBtnKey(uint8_t r, uint8_t c) {
  return (r == ENC_BTN_R && c == ENC_BTN_C);
}

static inline void setLayer(uint8_t l) {
  currentLayer = (l % LAYERS);
  Serial.print(F("LAYER -> "));
  Serial.println(currentLayer);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  for (uint8_t c = 0; c < COLS; c++) pinMode(colPins[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < ROWS; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH);
  }

  encoderInit();

#if ENABLE_HID
  BootKeyboard.begin();
#endif

  pixels.begin();
  pixels.setBrightness(LED_BRIGHTNESS);

  startupAnim();
  rgbRender();

  Serial.println(F("\n--- 2x3 MATRIX + ENCODER ---"));
  Serial.println(F("Encoder drehen: Layer wechseln (1 Rastung = 1 Layer)"));
  Serial.println(F("ENC-Button (Matrix r=0,c=2): RGB cycle MODE1->MODE2->OFF->ON"));
  Serial.print(F("Start Layer: ")); Serial.println(currentLayer);
  printRgbState();
}

void loop() {
  const uint32_t now = millis();

  // ---------------------
  // Encoder: Rastung -> Layer
  // ---------------------
  int8_t step = encoderReadStep();
  if (step != 0) {
    encAcc += step;

    if (encAcc >= ENC_STEPS_PER_DETENT) {
      encAcc = 0;
      setLayer((currentLayer + 1) % LAYERS);
    } else if (encAcc <= -ENC_STEPS_PER_DETENT) {
      encAcc = 0;
      setLayer((currentLayer + LAYERS - 1) % LAYERS);
    }
  }

  // ---------------------
  // Matrix scannen
  // ---------------------
  for (uint8_t r = 0; r < ROWS; r++) {
    selectRow(r);

    for (uint8_t c = 0; c < COLS; c++) {
      bool reading = (digitalRead(colPins[c]) == LOW);

      if (reading != lastReading[r][c]) {
        lastReading[r][c] = reading;
        lastChangeMs[r][c] = now;
      }

      if ((now - lastChangeMs[r][c]) >= DEBOUNCE_MS) {
        if (stableState[r][c] != reading) {
          stableState[r][c] = reading;

          // ENC Button: RGB state cycle (nur bei PRESS)
          if (isEncBtnKey(r, c)) {
            if (stableState[r][c]) {
              nextRgbState();
              rgbRender(); // sofort sichtbar
            }
            // kein HID senden
          } else {
            // normale Matrix-Tasten
            Serial.print(stableState[r][c] ? F("PRESS  ") : F("RELEASE"));
            Serial.print(F("  L=")); Serial.print(currentLayer);
            Serial.print(F("  r=")); Serial.print(r);
            Serial.print(F(" c=")); Serial.println(c);

#if ENABLE_HID
            KeyboardKeycode kc = keymap[currentLayer][r][c];
            if (stableState[r][c]) BootKeyboard.press(kc);
            else                   BootKeyboard.release(kc);
#endif
          }
        }
      }

      // Encoder auch im Scan pollen (stabiler)
      step = encoderReadStep();
      if (step != 0) {
        encAcc += step;
        if (encAcc >= ENC_STEPS_PER_DETENT) {
          encAcc = 0;
          setLayer((currentLayer + 1) % LAYERS);
        } else if (encAcc <= -ENC_STEPS_PER_DETENT) {
          encAcc = 0;
          setLayer((currentLayer + LAYERS - 1) % LAYERS);
        }
      }
    }
  }

  // ---------------------
  // RGB breathing kontinuierlich rendern
  // ---------------------
  rgbRender();
}

/*
PIN-KRITIK (damit du nicht wieder Geisterfehler jagst):
- Du willst (r=0,c=2) auf A0 -> colPins[2]=A0 ist korrekt.
- Wenn du vorher ENC_A=14/ENC_B=15 hattest, kollidiert das oft mit A0/A1.
  Darum hier ENC_A=8, ENC_B=9 (musst du so verdrahten).
- Wenn dein Encoder physisch auf A0/A1 hängt, musst du ENTWEDER:
  a) Encoder umverdrahten auf Digitalpins ODER
  b) Matrix-Pin weg von A0.
*/