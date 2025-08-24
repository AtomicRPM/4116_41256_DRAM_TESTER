// 4116_41256_DRAM_TESTER
// ATOMICRPM
// 08-23-2025
//
// yo bitch ass dram SUCKS!

#include <Bounce2.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "gfx.h"

// ======= SPEED PACK: enable fast port I/O on Nano Mini (ATmega328P) =======
#define USE_FAST_IO 1

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1   // wire OLED RST to Arduino RESET

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- Debouncers -------------
Bounce bStart = Bounce();  // TEST button (A3 = 17)
Bounce bM     = Bounce();  // Memory-type switch (M_TYPE)
Bounce bD     = Bounce();  // Duration-type switch (D_TYPE)

// ---------------- Pin Map ----------------
// DRAM I/O & control 
#define DI  12   // Data In  (to DRAM) -> D12 = PB4
#define DO  13   // Data Out (from DRAM)-> D13 = PB5
#define WE  14   // A0 = PC0 (active LOW)
#define CAS 15   // A1 = PC1 (active LOW)
#define RAS 16   // A2 = PC2 (active LOW)

// Address bus
#define XA0 0    // D0 = PD0
#define XA1 1    // D1 = PD1
#define XA2 2    // D2 = PD2
#define XA3 3    // D3 = PD3
#define XA4 4    // D4 = PD4
#define XA5 5    // D5 = PD5
#define XA6 6    // D6 = PD6
#define XA7 7    // D7 = PD7
#define XA8 8    // D8 = PB0

// Switches (jumper/switch) use quality ones.
#define M_TYPE 9   // 41256 when HIGH, 4164 when LOW
#define D_TYPE 10  // Long when HIGH, Short when LOW

// Make sure you use a solid socket for the mini.
// Bad pin connections can make it appear the switches are not working.

#define BUS_SIZE 9

volatile int bus_size = BUS_SIZE;  // runtime-selected width
bool test_complete = true;         // idle when true

// Latched states for the run (switch flips mid-run are ignored)
bool latchedLongMode = false;
bool latchedIs41256  = true;

const uint8_t a_bus[BUS_SIZE] = {
  XA0, XA1, XA2, XA3, XA4, XA5, XA6, XA7, XA8
};

// -------------- FAST I/O (no rewiring) --------------
#if USE_FAST_IO
// Port bit definitions per your wiring
static const uint8_t PB_A8  = _BV(0);  // D8  -> XA8
static const uint8_t PB_DI  = _BV(4);  // D12 -> DI
static const uint8_t PB_DO  = _BV(5);  // D13 -> DO

static const uint8_t PC_WE  = _BV(0);  // A0 -> WE  (active LOW)
static const uint8_t PC_CAS = _BV(1);  // A1 -> CAS (active LOW)
static const uint8_t PC_RAS = _BV(2);  // A2 -> RAS (active LOW)

static inline void ctrl_idle() {
  PORTC |= (PC_WE | PC_CAS | PC_RAS);   // all HIGH (inactive)
}

static inline void addr_set(uint16_t a) {
  PORTD = (uint8_t)a;                    // XA0..XA7 on PD0..PD7
  if (a & 0x100) PORTB |= PB_A8; else PORTB &= ~PB_A8;  // XA8 on PB0
}

static inline void di_write(uint8_t bit) {
  if (bit) PORTB |=  PB_DI; else PORTB &= ~PB_DI;
}

static inline uint8_t do_read() {
  return (PINB & PB_DO) ? 1 : 0;
}

// small inline nop helpers @16MHz (1 cycle = 62.5ns)
#define NOP()  __asm__ __volatile__("nop\n\t")
#define NOP2() do { NOP(); NOP(); } while(0)
#define NOP3() do { NOP(); NOP(); NOP(); } while(0)

// Safe, still-fast DRAM cycles (tRCD/tCAC/tWP/tDH margins)
static inline void writeAddress_fast(uint16_t r, uint16_t c, uint8_t v) {
  // Present ROW and assert RAS (start row cycle)
  addr_set(r);
  PORTC &= ~PC_RAS;          // RAS LOW
  NOP3();                    // tRCD ~187ns

  // Enter write, set data, meet setup before CAS
  PORTC &= ~PC_WE;           // WE LOW
  di_write(v & 1);
  NOP2();                    // data setup

  // Present COL and assert CAS (latch point)
  addr_set(c);
  PORTC &= ~PC_CAS;          // CAS LOW
  NOP2();                    // tWP/tDH

  // Close write
  PORTC |= PC_WE;            // WE HIGH
  NOP();
  PORTC |= PC_CAS;           // CAS HIGH
  NOP2();                    // small RAS precharge margin
  PORTC |= PC_RAS;           // RAS HIGH
}

static inline uint8_t readAddress_fast(uint16_t r, uint16_t c) {
  uint8_t ret;

  // Present ROW and assert RAS
  addr_set(r);
  PORTC &= ~PC_RAS;          // RAS LOW
  NOP3();                    // tRCD

  // Present COL and assert CAS
  addr_set(c);
  PORTC &= ~PC_CAS;          // CAS LOW
  NOP3();                    // tCAC ~187ns

  // Sample DO
  ret = do_read();

  // Close read
  NOP();
  PORTC |= PC_CAS;           // CAS HIGH
  NOP2();
  PORTC |= PC_RAS;           // RAS HIGH
  return ret;
}
#endif

// ---------------- Prototypes -------------
void startTesting();
void doShortTestLoop();  // March C-
void doLongTestLoop();   // Composite
void showIdleScreen();

void writeAddress(unsigned int r, unsigned int c, int v);
int  readAddress(unsigned int r, unsigned int c);

void error(int r, int c);
void ok(void);
void waitForTestButton();

void ui_splash();
void clear_info_area();
void display_status(const __FlashStringHelper* line1, const __FlashStringHelper* line2 = nullptr);
void display_progress_bar(uint8_t percent);
void display_step(const __FlashStringHelper* label, unsigned long cur, unsigned long total);

enum Dir { DIR_UP, DIR_DN };
bool march_pass(Dir dir, int read_expect, int write_val, uint8_t pass_idx);
bool run_marchC();
static inline void print_pass_name(uint8_t idx);
static inline void show_short_pass_header(uint8_t pass_idx);

// Composite helpers
static inline void ui_tick(unsigned long k, unsigned long TOTAL);
static bool run_checkerboard(bool inverse, unsigned long &k, unsigned long TOTAL);
static bool verify_checkerboard(bool inverse, unsigned long &k, unsigned long TOTAL);
static bool fill_solid(int v, unsigned long &k, unsigned long TOTAL);
static bool walking_phase(int bit_to_walk /*0 or 1*/, unsigned long &k, unsigned long TOTAL);
static void refresh_for_ms(uint16_t ms);
static bool retention_phase(uint16_t ms, bool inverse, unsigned long &k, unsigned long TOTAL);

// ---------------- Setup ------------------
void setup() {
  // Faster I2C for OLED
  Wire.setClock(400000);

  // TEST button on A3 (17) with pull-up
  pinMode(17, INPUT_PULLUP);
  bStart.attach(17);
  bStart.interval(5);

  // Address lines
  for (int i = 0; i < BUS_SIZE; i++) pinMode(a_bus[i], OUTPUT);

  // Control and data lines
  pinMode(CAS, OUTPUT);
  pinMode(RAS, OUTPUT);
  pinMode(WE,  OUTPUT);
  pinMode(DI,  OUTPUT);
  pinMode(DO,  INPUT);

#if USE_FAST_IO
  ctrl_idle(); // ensure WE/CAS/RAS start HIGH
#else
  digitalWrite(WE,  HIGH);
  digitalWrite(CAS, HIGH);
  digitalWrite(RAS, HIGH);
#endif

  // Switches (keep INPUT if you have external resistors; use INPUT_PULLUP if SPST to GND)
  pinMode(M_TYPE, INPUT);
  pinMode(D_TYPE, INPUT);

  bM.attach(M_TYPE); bM.interval(5);
  bD.attach(D_TYPE); bD.interval(5);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;); // hang if OLED not found
  }
  display.clearDisplay();
  ui_splash();
  showIdleScreen();
}

// ---------------- Main Loop --------------
void loop() {
  bStart.update();
  bM.update();
  bD.update();

  // Start test on button press (only when idle)
  if (test_complete && bStart.fell()) {
    startTesting();
  }

  // While idle, reflect live switch changes on OLED
  if (test_complete && (bM.changed() || bD.changed())) {
    showIdleScreen();
  }

  // Run selected test while active
  if (!test_complete) {
    if (latchedLongMode) doLongTestLoop();
    else                 doShortTestLoop();
  }
}

// -------------- Start Test ---------------
void startTesting() {
  // Latch debounced switch states (so mid-run flips don't affect this run)
  latchedLongMode = bD.read();   // HIGH = Long, LOW = Short
  latchedIs41256  = bM.read();   // HIGH = 41256, LOW = 4164

  // Pick bus width from latched memory type
  bus_size = latchedIs41256 ? BUS_SIZE : (BUS_SIZE - 1);

  display.clearDisplay();
  display.drawBitmap(0, 0, testing1, 128, 64, 1);
  display.display();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.print(latchedIs41256 ? F("41256 DRAM") : F("4164 DRAM"));
  display.display();

  // Precharge / init control lines
#if USE_FAST_IO
  ctrl_idle();
#else
  digitalWrite(WE,  HIGH);
  digitalWrite(RAS, HIGH);
  digitalWrite(CAS, HIGH);
#endif

  noInterrupts();
  for (int i = 0; i < (1 << bus_size); i++) {
#if USE_FAST_IO
    PORTC &= ~_BV(2); // RAS LOW
    PORTC |=  _BV(2); // RAS HIGH
#else
    digitalWrite(RAS, LOW);
    digitalWrite(RAS, HIGH);
#endif
  }
  interrupts();

  test_complete = false;
}

// -------- Short Test = March C- ----------
void doShortTestLoop() {
  // Header shown per-pass inside march_pass()
  bool ok_all = run_marchC();

  interrupts();
  if (ok_all) {
    display_progress_bar(100);
    display.display();
    ok();
  }
  // on failure, error() already showed UI and set test_complete=true
}

// -------- Long Test (Composite) --------
void doLongTestLoop() {
  const unsigned int N = (1u << bus_size);
  const unsigned long TOTAL = (unsigned long)N * N;
  unsigned long k = 0;

  // Phase 1: March C-
  interrupts();
  display_status(F("Long: Composite"), F("Phase 1/5: March C-"));
  display_progress_bar(0);
  display.display();
  noInterrupts();
  if (!run_marchC()) { return; }

  // ---- Phase 2: Checkerboard ----
  // Write
  interrupts();
  display_status(F("Long: Composite"), F("Phase 2/5: Checkerboard (write)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;
  if (!run_checkerboard(false, k, TOTAL)) return;

  // Verify
  interrupts();
  display_status(F("Long: Composite"), F("Phase 2/5: Checkerboard (verify)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;  // reset before verify
  if (!verify_checkerboard(false, k, TOTAL)) return;


  // ---- Phase 3: Inverse Checkerboard ----
  // Write
  interrupts();
  display_status(F("Long: Composite"), F("Phase 3/5: Inverse Checker (write)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;
  if (!run_checkerboard(true, k, TOTAL)) return;

  // Verify
  interrupts();
  display_status(F("Long: Composite"), F("Phase 3/5: Inverse Checker (verify)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;  // reset before verify
  if (!verify_checkerboard(true, k, TOTAL)) return;


  // ---- Phase 4: Walking-1 over 0 ----
  interrupts();
  display_status(F("Long: Composite"), F("Phase 4/5: Walking-1 (over 0)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;
  if (!fill_solid(0, k, TOTAL)) return;    // background 0

  k = 0;  // reset before the walking pass
  if (!walking_phase(1, k, TOTAL)) return; // walk 1's


  // ---- Phase 4: Walking-0 over 1 ----
  interrupts();
  display_status(F("Long: Composite"), F("Phase 4/5: Walking-0 (over 1)"));
  display_progress_bar(0);
  display.display();
  noInterrupts();

  k = 0;
  if (!fill_solid(1, k, TOTAL)) return;    // background 1

  k = 0;  // reset before the walking pass
  if (!walking_phase(0, k, TOTAL)) return; // walk 0's


  // Phase 5: Retention (quick soak with refresh) — both polarities
  interrupts();
  display_status(F("Long: Composite"), F("Phase 5/5: Retention"));
  display_progress_bar(0);
  display.display();
  
  k = 0;
  if (!retention_phase(300, false, k, TOTAL)) return;
  if (!retention_phase(300, true , k, TOTAL)) return;

  // All phases done
  interrupts();
  ok();
}

// -------------- DRAM Helpers -------------
void writeAddress(unsigned int r, unsigned int c, int v) {
#if USE_FAST_IO
  writeAddress_fast(r, c, (uint8_t)(v & 1));
#else
  // Safe, portable (slower) path
  // set row
  for (int i = 0; i < bus_size; i++) { digitalWrite(a_bus[i], (r & 1) ? HIGH : LOW); r >>= 1; }
  digitalWrite(RAS, LOW);

  // write mode + data
  digitalWrite(WE, LOW);
  digitalWrite(DI, (v & 1) ? HIGH : LOW);

  // set col
  for (int i = 0; i < bus_size; i++) { digitalWrite(a_bus[i], (c & 1) ? HIGH : LOW); c >>= 1; }
  digitalWrite(CAS, LOW);

  digitalWrite(WE, HIGH);
  digitalWrite(CAS, HIGH);
  digitalWrite(RAS, HIGH);
#endif
}

int readAddress(unsigned int r, unsigned int c) {
#if USE_FAST_IO
  return (int)readAddress_fast(r, c);
#else
  int ret = 0;
  // set row
  for (int i = 0; i < bus_size; i++) { digitalWrite(a_bus[i], (r & 1) ? HIGH : LOW); r >>= 1; }
  digitalWrite(RAS, LOW);

  // set col
  for (int i = 0; i < bus_size; i++) { digitalWrite(a_bus[i], (c & 1) ? HIGH : LOW); c >>= 1; }
  digitalWrite(CAS, LOW);

  ret = digitalRead(DO);

  digitalWrite(CAS, HIGH);
  digitalWrite(RAS, HIGH);
  return ret;
#endif
}

// ----------------- UI --------------------
void ui_splash() {
  display.drawBitmap(0, 0, eddie1, 128, 64, 1);
  display.display();
  delay(1200);
  display.clearDisplay();
  display.display();
}

void clear_info_area() {
  display.fillRect(0, 24, 128, 40, SSD1306_BLACK);
}

void display_status(const __FlashStringHelper* line1, const __FlashStringHelper* line2) {
  clear_info_area();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 26);
  display.println(line1);
  if (line2 != nullptr) {
    display.setCursor(0, 36);
    display.println(line2);
  }
}

void display_progress_bar(uint8_t percent) {
  const int x = 4, y = 54, w = 120, h = 8;
  display.drawRect(x-1, y-1, w+2, h+2, SSD1306_WHITE);
  int fillw = (int)((percent * w) / 100);
  if (fillw < 0) fillw = 0; if (fillw > w) fillw = w;
  display.fillRect(x, y, fillw, h, SSD1306_WHITE);
  display.fillRect(90, 26, 38, 10, SSD1306_BLACK);
  display.setCursor(90, 26);
   display.print(F(" "));
  display.print(percent);
  display.print(F("%"));
}

void display_step(const __FlashStringHelper* label, unsigned long cur, unsigned long total) {
  display.fillRect(0, 44, 128, 10, SSD1306_BLACK);
  display.setCursor(0, 44);
  display.print(label);
  display.print(F(" "));
  display.print(cur);
  display.print(F("/"));
  display.print(total);
}

void showIdleScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("   DRAM"));
  display.println(F("  TESTER"));
  display.setTextSize(1);

  const bool memIs41256 = bM.read();   // debounced
  const bool longMode   = bD.read();   // debounced

  display.println(memIs41256 ? F("41256 Test Selected")
                             : F(" 4164 Test Selected"));
  display.println(longMode   ? F("Long Test (Composite)")
                             : F("Short Test: March C-"));

  display.println(F(""));
  display.println(F("    Press TEST to"));
  display.println(F("    Start Testing"));
  display.display();
}

// ----------------- Error / OK ------------
void error(int r, int c) {
  test_complete = true;

  unsigned long a = ((unsigned long)((unsigned)c) << bus_size) + (unsigned long)r;

  interrupts();
  display.clearDisplay();
  display.drawBitmap(0, 0, failed1, 128, 64, 1);
  display.display();
  delay(1200);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("  DRAM BAD"));
  display.setTextSize(1);
  String aString = String(a, HEX);
  display.println(F(""));
  display.print(F("   FAILED AT "));
  display.print(aString);
  display.println(F(""));
  display.println(F(""));
  display.println(F(" Press TEST to"));
  display.println(F("   continue"));
  display.display();

  waitForTestButton();
  showIdleScreen();
}

void ok(void) {
  test_complete = true;

  interrupts();
  display.clearDisplay();
  display.drawBitmap(0, 0, passed1, 128, 64, 1);
  display.display();
  delay(1200);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("   DRAM OK"));
  display.setTextSize(1);
  display.println(F(""));
  display.println(F(" Press TEST to"));
  display.println(F("   continue"));
  display.display();

  waitForTestButton();
  showIdleScreen();
}

// Wait until the TEST button is pressed (and released)
void waitForTestButton() {
  bStart.update();
  // ensure release first
  while (bStart.read() == LOW) { bStart.update(); }
  // wait for a clean press
  for (;;) {
    bStart.update();
    if (bStart.fell()) {
      while (bStart.read() == LOW) bStart.update();
      break;
    }
  }
}


static inline void print_pass_name(uint8_t idx) {
  switch (idx) {
    case 0: display.print(F("UP  W0"));       break;
    case 1: display.print(F("UP  R0,W1"));    break;
    case 2: display.print(F("UP  R1,W0"));    break;
    case 3: display.print(F("DN  R0,W1"));    break;
    case 4: display.print(F("DN  R1,W0"));    break;
    case 5: display.print(F("DN  R0"));       break;
    default:display.print(F(""));             break;
  }
}

static inline void show_short_pass_header(uint8_t pass_idx) {
  clear_info_area();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 26);
  display.println(F("Short: March C-"));
  display.setCursor(0, 36);
  display.print(F("Pass "));
  display.print(pass_idx + 1);
  display.print(F("/6  "));
  print_pass_name(pass_idx);
  display_progress_bar(0);
  display.display();
}

// Iterate whole address space in the given direction, optionally read &/or write each cell.
// dir: DIR_UP / DIR_DN
// read_expect: -1 = RX (skip read), 0 = R0, 1 = R1
// write_val  : -1 = WX (skip write), 0 = W0, 1 = W1
bool march_pass(Dir dir, int read_expect, int write_val, uint8_t pass_idx) {
  const unsigned int N = (1u << bus_size);
  const unsigned long TOTAL = (unsigned long)N * N;
  unsigned long k = 0;

  interrupts();
  show_short_pass_header(pass_idx);
  noInterrupts();

  auto step_cell = [&](unsigned int r, unsigned int c) -> bool {
    if (read_expect >= 0) {
      int v = readAddress(r, c);
      if (v != (read_expect & 1)) { error(r, c); return false; }
    }
    if (write_val >= 0) {
      writeAddress(r, c, write_val & 1);
    }
    return true;
  };

  if (dir == DIR_UP) {
    for (unsigned int r = 0; r < N; ++r) {
      for (unsigned int c = 0; c < N; ++c) {
        if (!step_cell(r, c)) return false;
        if ((++k & 0xFFUL) == 0) {
          interrupts();
          uint8_t pct = (uint8_t)((k * 100UL) / (TOTAL ? TOTAL : 1));
          display_step(F("Cells"), k, TOTAL);
          display_progress_bar(pct);
          display.display();
          noInterrupts();
        }
      }
    }
  } else { // DIR_DN
    for (int r = (int)N - 1; r >= 0; --r) {
      for (int c = (int)N - 1; c >= 0; --c) {
        if (!step_cell((unsigned)r, (unsigned)c)) return false;
        if ((++k & 0xFFUL) == 0) {
          interrupts();
          uint8_t pct = (uint8_t)((k * 100UL) / (TOTAL ? TOTAL : 1));
          display_step(F("Cells"), k, TOTAL);
          display_progress_bar(pct);
          display.display();
          noInterrupts();
        }
      }
    }
  }

  interrupts();
  display_step(F("Cells"), TOTAL, TOTAL);
  display_progress_bar(100);
  display.display();
  noInterrupts();

  return true;
}

bool run_marchC() {
  if (!march_pass(DIR_UP, -1, 0, 0)) return false;  // UP:  W0
  if (!march_pass(DIR_UP,  0, 1, 1)) return false;  // UP:  R0,W1
  if (!march_pass(DIR_UP,  1, 0, 2)) return false;  // UP:  R1,W0
  if (!march_pass(DIR_DN,  0, 1, 3)) return false;  // DN:  R0,W1
  if (!march_pass(DIR_DN,  1, 0, 4)) return false;  // DN:  R1,W0
  if (!march_pass(DIR_DN,  0, -1,5)) return false;  // DN:  R0
  return true;
}

// -------------- Composite helpers --------------
static inline void ui_tick(unsigned long k, unsigned long TOTAL) {
  if ((k & 0xFFUL) == 0) {
    interrupts();
    unsigned long pctL = (TOTAL ? (k * 100UL) / TOTAL : 0UL);
    uint8_t pct = (pctL > 100UL) ? 100 : (uint8_t)pctL;  // clamp
    display_step(F("Cells"), k, TOTAL);
    display_progress_bar(pct);
    display.display();
    noInterrupts();
  }
}


// Write Checkerboard pattern (inverse=false -> (r^c)&1, true -> ^1)
static bool run_checkerboard(bool inverse, unsigned long &k, unsigned long TOTAL) {
  const unsigned int N = (1u << bus_size);
  for (unsigned int r = 0; r < N; ++r) {
    for (unsigned int c = 0; c < N; ++c) {
      int v = ((r ^ c) & 1) ^ (inverse ? 1 : 0);
      writeAddress(r, c, v);
      ++k; ui_tick(k, TOTAL);
    }
  }
  return true;
}
static bool verify_checkerboard(bool inverse, unsigned long &k, unsigned long TOTAL) {
  const unsigned int N = (1u << bus_size);
  for (unsigned int r = 0; r < N; ++r) {
    for (unsigned int c = 0; c < N; ++c) {
      int v = ((r ^ c) & 1) ^ (inverse ? 1 : 0);
      if (readAddress(r, c) != v) { error(r, c); return false; }
      ++k; ui_tick(k, TOTAL);
    }
  }
  return true;
}

// Fill all cells to solid 0 or 1, then sanity-verify once
static bool fill_solid(int v, unsigned long &k, unsigned long TOTAL) {
  const unsigned int N = (1u << bus_size);
  for (unsigned int r = 0; r < N; ++r)
    for (unsigned int c = 0; c < N; ++c) { writeAddress(r, c, v & 1); ++k; ui_tick(k, TOTAL); }

  k = 0; // reuse bar for verify
  for (unsigned int r = 0; r < N; ++r)
    for (unsigned int c = 0; c < N; ++c) { if (readAddress(r, c) != (v & 1)) { error(r, c); return false; } ++k; ui_tick(k, TOTAL); }
  return true;
}

// Walking-1 (bit_to_walk=1) or Walking-0 (bit_to_walk=0) across entire plane
// Background should be pre-set (0 for walk-1, 1 for walk-0).
static bool walking_phase(int bit_to_walk, unsigned long &k, unsigned long TOTAL) {
  const unsigned int N = (1u << bus_size);
  const int bg = bit_to_walk ? 0 : 1;
  const int fg = bit_to_walk ? 1 : 0;

  for (unsigned int r = 0; r < N; ++r) {
    for (unsigned int c = 0; c < N; ++c) {
      // set the single cell to fg, verify, then restore bg
      writeAddress(r, c, fg);
      if (readAddress(r, c) != fg) { error(r, c); return false; }
      writeAddress(r, c, bg);
      ++k; ui_tick(k, TOTAL);
    }
  }
  return true;
}

// Issue RAS-only refresh for each row in a cycle, for approx `ms` milliseconds
static void refresh_for_ms(uint16_t ms) {
  const unsigned int N = (1u << bus_size);
  unsigned long t_end = millis() + ms;

#if USE_FAST_IO
  // Ensure WE/CAS HIGH
  PORTC |= (PC_WE | PC_CAS);
  while ((long)(millis() - t_end) < 0) {
    for (unsigned int r = 0; r < N; ++r) {
      // Present row, RAS low pulse, CAS stays HIGH, WE HIGH
      PORTD = (uint8_t)r;
      if (bus_size > 8) { if (r & 0x100) PORTB |= PB_A8; else PORTB &= ~PB_A8; }
      PORTC &= ~PC_RAS;   // RAS LOW
      NOP2();             // tiny pulse
      PORTC |=  PC_RAS;   // RAS HIGH
    }
  }
#else
  while ((long)(millis() - t_end) < 0) {
    for (unsigned int r = 0; r < N; ++r) {
      // Present row on slow path
      unsigned int tmp=r;
      for (int i=0;i<bus_size;i++){ digitalWrite(a_bus[i], (tmp&1)?HIGH:LOW); tmp>>=1; }
      digitalWrite(RAS, LOW);
      delayMicroseconds(1);
      digitalWrite(RAS, HIGH);
    }
  }
#endif
}

// Retention: write checkerboard (or inverse), refresh for `ms`, verify
static bool retention_phase(uint16_t ms, bool inverse, unsigned long &k, unsigned long TOTAL) {
  // Write pattern
  k = 0;
  if (!run_checkerboard(inverse, k, TOTAL)) return false;

  // Refresh while “waiting”
  interrupts();
  display_step(F("Refresh"), 0, ms);
  display_progress_bar(0);
  display.display();
  // keep interrupts ON so millis() advances

// Keep interrupts ON so millis() advances inside refresh_for_ms()
const uint16_t step = 25;
for (uint16_t t=0; t<ms; t+=step) {
  refresh_for_ms(step);  // uses millis()
  uint16_t shown = (t+step > ms) ? ms : (t+step);
  interrupts();
  display_step(F("Refresh"), shown, ms);
  display_progress_bar((uint8_t)(((unsigned long)shown * 100UL)/ms));
  display.display();
  // no need to call noInterrupts() here
}

  // Verify pattern
  k = 0;
  if (!verify_checkerboard(inverse, k, TOTAL)) return false;

  return true;
}
