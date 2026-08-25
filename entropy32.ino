/*
 * Entropy32 - "The Universe Bifurcator"
 * ---------------------------------------------------------
 * Geiger-pulse based TRNG -> BIP39 seed phrase generator
 *
 * Hardware:
 *   - ATmega328P
 *   - 16x2 LCD via I2C backpack (SDA=A4, SCL=A5)
 *   - Processed Geiger pulses (post-LM393) on D2 (INT0)
 *   - Back button  -> D4 (active LOW, internal pull-up)
 *   - Forward button -> D5 (active LOW, internal pull-up)
 *
 * Entropy method:
 *   1. Capture inter-arrival time between successive Geiger pulses.
 *   2. Compare each interval to the previous one:
 *        longer  -> bit 1
 *        shorter -> bit 0
 *        equal   -> discarded (extremely rare at microsecond resolution)
 *   3. Collect a raw pool of comparison-bits (RAW_POOL_BITS).
 *   4. Whiten/condition the raw pool with SHA-256 to remove any
 *      residual structure (dead-time correlation, count-rate drift, etc).
 *   5. Follow the standard BIP39 process on the conditioned entropy:
 *      compute checksum = first ENT/32 bits of SHA256(entropy),
 *      append it, split into 11-bit chunks, map each chunk to a
 *      word in the official 2048-word list.
 *
 * IMPORTANT / CAVEAT:
 *   This is a hobbyist/prototype entropy source. Before trusting output
 *   from this device for a seed that will secure real funds:
 *     - Log raw inter-arrival times over a long run and evaluate them
 *       with the NIST SP 800-90B non-IID min-entropy estimators (not
 *       just SP 800-22 pass/fail randomness tests) to confirm the raw
 *       pool has genuinely enough min-entropy for what you're claiming.
 *     - Consider mixing this source with a second, independently-
 *       validated entropy source (e.g. your avalanche-noise Bifurcator)
 *       before finalizing a seed for anything holding real value.
 *     - Never transcribe a generated phrase for real funds directly off
 *       a device you haven't independently audited end-to-end.
 *
 * Required library:
 *   - LiquidCrystal_I2C (e.g. by johnrickman / Frank de Brabander)
 *     Install via Arduino IDE Library Manager.
 *
 * Required companion files (same sketch folder):
 *   - sha256.h / sha256.cpp   (included, self-contained, no dependency)
 *   - bip39_wordlist.h        (generate with generate_wordlist.py)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "sha256.h"
#include "bip39_wordlist.h"
#include "button.h"

// ---------------- Test Mode ----------------
#define TEST_MODE 1

// ---------------- Pin assignments ----------------
#define GEIGER_PIN 2   // INT0 - processed pulse edge from LM393
#define BACK_PIN   4
#define FWD_PIN    5

// ---------------- LCD ----------------
// Common addresses are 0x27 or 0x3F - run an I2C scanner sketch once
// if the display doesn't init, and adjust here.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- Entropy collection ----------------
#define RAW_POOL_BITS    512     // raw comparison-bits collected before offering the menu
#define MIN_INTERVAL_US  200     // reject intervals shorter than this (debounce/glitch guard)

volatile uint8_t  entropyPool[RAW_POOL_BITS / 8]; // bit-packed raw pool
volatile uint16_t poolBitIndex   = 0;
volatile unsigned long lastPulseMicros = 0;
volatile unsigned long prevInterval    = 0;
volatile bool intervalValid = false;

void geigerISR() {
  unsigned long now = micros();
  if (lastPulseMicros != 0) {
    unsigned long interval = now - lastPulseMicros;
    if (interval >= MIN_INTERVAL_US) {
      if (intervalValid && poolBitIndex < RAW_POOL_BITS) {
        if (interval != prevInterval) {
          uint8_t bit = (interval > prevInterval) ? 1 : 0;
          uint16_t byteIdx   = poolBitIndex >> 3;
          uint8_t  bitOffset = poolBitIndex & 0x07;
          if (bit) entropyPool[byteIdx] |=  (1 << bitOffset);
          else     entropyPool[byteIdx] &= ~(1 << bitOffset);
          poolBitIndex++;
        }
      }
      prevInterval = interval;
      intervalValid = true;
    }
    lastPulseMicros = now;
  } else {
    lastPulseMicros = now;
  }
}

// ---------------- Buttons (polled, debounced) ----------------
Button backBtn = { BACK_PIN, HIGH, 0 };
Button fwdBtn  = { FWD_PIN,  HIGH, 0 };
#define DEBOUNCE_MS 30

bool buttonPressed(Button &b) {
  bool reading = digitalRead(b.pin);
  bool pressed = false;

  if (reading != b.lastState &&
      (millis() - b.lastChange) > DEBOUNCE_MS) {

    b.lastChange = millis();

    if (reading == LOW)
      pressed = true;

    b.lastState = reading;
  }

  return pressed;
}


// ---------------- Menu buttons ----------------
//
// BACK alone   = previous
// FWD alone    = next
// BACK + FWD   = select

#define COMBO_WINDOW_MS 150

enum MenuAction {
  MENU_NONE,
  MENU_BACK,
  MENU_FWD,
  MENU_SELECT
};

MenuAction readMenuAction();

bool menuComboPending = false;
bool menuComboHandled = false;
bool menuFirstWasBack = false;
unsigned long menuFirstPressMs = 0;

MenuAction readMenuAction() {

  bool backDown = (digitalRead(BACK_PIN) == LOW);
  bool fwdDown  = (digitalRead(FWD_PIN)  == LOW);

  // Both buttons pressed = SELECT
  if (backDown && fwdDown) {
    if (!menuComboHandled) {
      menuComboHandled = true;
      return MENU_SELECT;
    }
    return MENU_NONE;
  }

  // Wait for both buttons to be released after SELECT
  if (menuComboHandled) {
    if (!backDown && !fwdDown) {
      menuComboHandled = false;
    }
    return MENU_NONE;
  }

  // A button is currently held down.
  // Don't generate another action until it is released.
  if (backDown || fwdDown) {
    if (!menuComboPending) {
      menuComboPending = true;
      menuFirstWasBack = backDown;
      menuFirstPressMs = millis();
    }

    // If the other button joins during the combo window,
    // it becomes SELECT.
    if (backDown && fwdDown) {
      menuComboPending = false;
      menuComboHandled = true;
      return MENU_SELECT;
    }

    // Single button held long enough = one navigation action.
    if (millis() - menuFirstPressMs >= COMBO_WINDOW_MS) {
      menuComboPending = false;
      return menuFirstWasBack ? MENU_BACK : MENU_FWD;
    }

    return MENU_NONE;
  }

  // Both released.
  menuComboPending = false;

  return MENU_NONE;
}

// ---------------- App state machine ----------------
enum AppState {
  STATE_COLLECTING,
  STATE_MENU_LENGTH,
  STATE_SHOW_WORD,
  STATE_DONE
};
AppState state = STATE_COLLECTING;

uint8_t  selectedLength  = 12;   // toggled between 12 / 24 in the menu
uint8_t  wordCount       = 0;
uint16_t wordIndices[24];
uint8_t  currentWordPos  = 0;

// ---------------- SHA-256 boot self-test ----------------
// Known-answer test vector: SHA-256("abc")
// Independently verifiable, e.g.:
//   echo -n "abc" | openssl dgst -sha256
//   python3 -c "import hashlib; print(hashlib.sha256(b'abc').hexdigest())"
const uint8_t KAT_INPUT[3] PROGMEM = { 'a', 'b', 'c' };
const uint8_t KAT_EXPECTED[32] PROGMEM = {
  0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
  0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
};

// Runs the KAT and halts on failure (infinite loop, does not return)
// with a FAIL screen. On success, briefly shows PASS + a short hash
// fingerprint before continuing into normal operation.
void runSHA256SelfTest() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SHA-256 self-");
  lcd.setCursor(0, 1);
  lcd.print("test running...");

  uint8_t katInputRam[3];
  memcpy_P(katInputRam, KAT_INPUT, sizeof(katInputRam));

  SHA256 sha;
  sha.update(katInputRam, sizeof(katInputRam));
  uint8_t digest[32];
  sha.finalize(digest);

  bool pass = true;
  for (uint8_t i = 0; i < 32; i++) {
    uint8_t expectedByte = pgm_read_byte(&KAT_EXPECTED[i]);
    if (digest[i] != expectedByte) {
      pass = false;
      break;
    }
  }

  lcd.clear();
  if (pass) {
    lcd.setCursor(0, 0);
    lcd.print("SHA-256 test:");
    lcd.setCursor(0, 1);
    lcd.print("PASS  ");
    // Show first 4 hex bytes of the digest as a quick visual
    // fingerprint the user can cross-check against the published
    // vector (ba7816bf...) if they want extra confidence.
    for (uint8_t i = 0; i < 4; i++) {
      char hex[3];
      sprintf(hex, "%02x", digest[i]);
      lcd.print(hex);
    }
    delay(1800);
  } else {
    // Do not proceed. A broken conditioning step must never silently
    // feed into seed generation.
    lcd.setCursor(0, 0);
    lcd.print("SHA-256 test:");
    lcd.setCursor(0, 1);
    lcd.print("FAIL - HALTED");
    while (true) {
      // halt indefinitely; user must power-cycle after investigating
      delay(1000);
    }
  }
}

// ---------------- Setup ----------------
void setup() {
  pinMode(GEIGER_PIN, INPUT);
  pinMode(BACK_PIN, INPUT_PULLUP);
  pinMode(FWD_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, RISING);

  #if TEST_MODE
    // TESTING ONLY: skip entropy collection
    poolBitIndex = RAW_POOL_BITS;

    for (uint8_t i = 0; i < sizeof(entropyPool); i++) {
      entropyPool[i] = i * 37 + 123;
    }
  #endif

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Entropy32");
  lcd.setCursor(0, 1);
  lcd.print("Booting...");
  delay(600);

  runSHA256SelfTest(); // halts here if the SHA-256 implementation is broken

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Entropy32");
  lcd.setCursor(0, 1);
  lcd.print("Collecting...");
}

// ---------------- Main loop ----------------
void loop() {
  switch (state) {

    case STATE_COLLECTING:
      updateCollectingScreen();
      if (poolBitIndex >= RAW_POOL_BITS) {
        state = STATE_MENU_LENGTH;
        drawMenuScreen();
      }
      break;

    case STATE_MENU_LENGTH: {
      MenuAction action = readMenuAction();

      if (action == MENU_BACK) {
        selectedLength = (selectedLength == 12) ? 24 : 12;
        drawMenuScreen();

      } else if (action == MENU_FWD) {
        selectedLength = (selectedLength == 12) ? 24 : 12;
        drawMenuScreen();

      } else if (action == MENU_SELECT) {
        generatePhrase();
        state = STATE_SHOW_WORD;
        currentWordPos = 0;
        drawWordScreen();
      }

      break;
    }

    case STATE_SHOW_WORD:
      if (buttonPressed(fwdBtn)) {
        if (currentWordPos < wordCount - 1) {
          currentWordPos++;
          drawWordScreen();
        } else {
          state = STATE_DONE;
          drawDoneScreen();
        }
      }
      if (buttonPressed(backBtn)) {
        if (currentWordPos > 0) {
          currentWordPos--;
          drawWordScreen();
        }
      }
      break;

    case STATE_DONE:
      if (buttonPressed(backBtn)) {
        state = STATE_SHOW_WORD;
        currentWordPos = wordCount - 1;
        drawWordScreen();
      }
      break;
  }
}

// ---------------- Screens ----------------
unsigned long lastCollectDraw = 0;

void updateCollectingScreen() {
  if (millis() - lastCollectDraw > 250) { // throttle LCD writes
    lastCollectDraw = millis();
    lcd.setCursor(0, 1);
    lcd.print("Bits: ");
    lcd.print(poolBitIndex);
    lcd.print("/");
    lcd.print(RAW_POOL_BITS);
    lcd.print("   ");
  }
}

void drawMenuScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Seed length:");
  lcd.setCursor(0, 1);
  lcd.print(selectedLength);
}

void drawWordScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Word ");
  lcd.print(currentWordPos + 1);
  lcd.print("/");
  lcd.print(wordCount);
  lcd.setCursor(0, 1);
  char wordBuf[10];
  strcpy_P(wordBuf, (PGM_P)pgm_read_word(&(BIP39_WORDLIST[wordIndices[currentWordPos]])));
  lcd.print(wordBuf);
}

void drawDoneScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Seed complete.");
  lcd.setCursor(0, 1);
  lcd.print("BACK to review");
}

// ---------------- Entropy conditioning + BIP39 generation ----------------
void generatePhrase() {
  // Snapshot the pool with interrupts disabled so the ISR can't
  // modify it mid-copy.
  noInterrupts();
  uint8_t poolCopy[RAW_POOL_BITS / 8];
  memcpy(poolCopy, (const void*)entropyPool, sizeof(poolCopy));
  interrupts();

  // Step 1: whiten/condition the raw pool via SHA-256.
  SHA256 sha;
  sha.update(poolCopy, sizeof(poolCopy));
  uint8_t conditioned[32];
  sha.finalize(conditioned);

  uint8_t entropyLenBytes = (selectedLength == 24) ? 32 : 16; // 256 or 128 bits
  uint8_t entropyBytes[32];
  memcpy(entropyBytes, conditioned, entropyLenBytes);

  // Step 2: BIP39 checksum = first (ENT/32) bits of SHA256(entropy).
  SHA256 sha2;
  sha2.update(entropyBytes, entropyLenBytes);
  uint8_t checksumHash[32];
  sha2.finalize(checksumHash);

  uint8_t checksumBits = (entropyLenBytes * 8) / 32; // 4 bits (12w) or 8 bits (24w)

  // Step 3: concat entropy bits + checksum bits, split into 11-bit
  // word indices per the BIP39 spec.
  wordCount = (entropyLenBytes * 8 + checksumBits) / 11;

  for (uint8_t w = 0; w < wordCount; w++) {
    uint16_t idx = 0;
    for (uint8_t b = 0; b < 11; b++) {
      uint16_t bitPos = (uint16_t)w * 11 + b;
      bool bitVal;
      if (bitPos < (uint16_t)entropyLenBytes * 8) {
        bitVal = (entropyBytes[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
      } else {
        uint16_t cbit = bitPos - (uint16_t)entropyLenBytes * 8;
        bitVal = (checksumHash[cbit / 8] >> (7 - (cbit % 8))) & 1;
      }
      idx = (idx << 1) | (bitVal ? 1 : 0);
    }
    wordIndices[w] = idx;
  }

  // Wipe sensitive buffers we no longer need in RAM.
  memset(poolCopy, 0, sizeof(poolCopy));
  memset(conditioned, 0, sizeof(conditioned));
}