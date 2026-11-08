
#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_err.h>

// ====================================================
// HP KB-9970 BLUETOOTH V4.6.2
// ESP32-C3 SUPERMINI
//
// PS/2 DATA  -> GPIO4
// PS/2 CLOCK -> GPIO3
//
// HID: 8 ms predeterminados
// Watchdog: 15 segundos
// KEEP-ALIVE CPU experimental para power bank IP5306
//
// IMPORTANTE:
// El teclado HP esta alimentado experimentalmente
// a 3.3 V. Su alimentacion nominal es de 5 V.
// No introducir 5 V en los GPIO del ESP32.
// ====================================================

// -------------------- PINES -------------------------

constexpr uint8_t PIN_DATA = 4;
constexpr uint8_t PIN_CLOCK = 3;

// -------------------- COLAS -------------------------

constexpr uint16_t EDGE_N = 1024;
constexpr uint16_t EDGE_MASK = EDGE_N - 1;

constexpr uint16_t HID_N = 256;
constexpr uint16_t HID_MASK = HID_N - 1;

// -------------------- TIEMPOS -----------------------

constexpr uint32_t PS2_TX_QUIET_US = 20000;
constexpr uint32_t BLE_SETTLE_MS = 2000;
constexpr uint32_t LED_STABLE_MS = 250;
constexpr uint32_t CMD_TIMEOUT_MS = 150;
constexpr uint32_t LED_PARAM_TIMEOUT_MS = 250;

// ====================================================
// KEEP-ALIVE
//
// Cada 18 segundos inicia una ventana de actividad
// de 4 segundos.
//
// Dentro de esa ventana, cada iteracion realiza
// aproximadamente 1.5 ms de calculo intensivo.
//
// El loop sigue atendiendo PS/2, BLE y watchdog.
//
// Es un experimento: NO garantiza alcanzar el
// umbral de corriente del IP5306.
// ====================================================

constexpr uint32_t KEEP_INTERVAL_MS = 18000;
constexpr uint32_t KEEP_DURATION_MS = 4000;
constexpr uint32_t KEEP_SLICE_US = 1500;

bool keepAliveEnabled = true;
bool keepAliveActive = false;

uint32_t keepWindowStartMs = 0;
uint32_t keepPulseStartMs = 0;
uint32_t keepPulseCount = 0;

volatile uint32_t keepCalculation = 0x12345678;

// ====================================================
// 1. ESTRUCTURAS Y PROTOTIPOS
// ====================================================

struct PS2Edge {
  uint32_t timeUs;
  uint8_t clock;
  uint8_t data;
};

struct HIDEvent {
  uint16_t usage;
  uint32_t queuedMs;
  bool down;
  bool media;
};

bool popEdge(PS2Edge &e);
void processFall(const PS2Edge &e);
void processPS2Byte(uint8_t b);
void processKey(uint8_t c, bool ext, bool down);

uint8_t keyUsage(uint8_t c, bool ext);
uint16_t mediaUsage(uint8_t c, bool ext);

void resetFrame();
void resetParser();

void servicePS2();
void serviceHID();
void serviceCommands();
void serviceConnection();
void serviceKeepAlive();

void printStats();
void printHelp();
void serialCommands();

bool sendPS2Byte(uint8_t b);
void commandError(const char *why);

// ====================================================
// 2. BLUETOOTH
// ====================================================

// Conservar el nombre utilizado anteriormente.

HijelHID_BLEKeyboard keyboard(
  "HP KB9970 V3",
  "KB9970 DIY",
  100
);

// Bateria indicada como 100% solo de forma simulada.

volatile int8_t pairingStatus = -1;

void pairingDone(bool success) {
  pairingStatus = success ? 1 : 0;
}

// ====================================================
// 3. INTERRUPCION PS/2
// ====================================================

volatile PS2Edge edges[EDGE_N];

volatile uint16_t edgeW = 0;
volatile uint16_t edgeR = 0;

volatile uint32_t edgeDrops = 0;
volatile uint32_t lastEdgeUs = 0;

volatile bool transmitting = false;

void ARDUINO_ISR_ATTR clockISR() {
  if (transmitting) return;

  uint32_t now = micros();
  lastEdgeUs = now;

  uint16_t next = (edgeW + 1) & EDGE_MASK;

  if (next == edgeR) {
    ++edgeDrops;
    return;
  }

  edges[edgeW].timeUs = now;
  edges[edgeW].clock = digitalRead(PIN_CLOCK);
  edges[edgeW].data = digitalRead(PIN_DATA);

  edgeW = next;
}

bool popEdge(PS2Edge &e) {
  noInterrupts();

  if (edgeR == edgeW) {
    interrupts();
    return false;
  }

  uint16_t n = edgeR;

  e.timeUs = edges[n].timeUs;
  e.clock = edges[n].clock;
  e.data = edges[n].data;

  edgeR = (n + 1) & EDGE_MASK;

  interrupts();
  return true;
}

// ====================================================
// 4. COLA HID
// ====================================================

HIDEvent hidEvents[HID_N];

uint16_t hidW = 0;
uint16_t hidR = 0;

bool hidHeld[256] = {};
bool ps2Held[512] = {};

uint16_t physicalHeldCount = 0;
uint16_t mediaHeld = 0;

bool hidNeedsRelease = false;

uint32_t hidDrops = 0;
uint32_t hidSent = 0;
uint32_t ignored = 0;
uint32_t unmapped = 0;
uint32_t resyncs = 0;

uint32_t lastKeyMs = 0;

uint16_t maxQueue = 0;
uint32_t maxQueueWaitMs = 0;
uint64_t sumQueueWaitMs = 0;
uint32_t queueSamples = 0;

// Velocidad inicial de 8 ms.

uint8_t hidGapMs = 8;

bool debugKeys = false;

void clearHIDQueue() {
  memset(hidHeld, 0, sizeof(hidHeld));

  mediaHeld = 0;
  hidR = hidW;
  hidNeedsRelease = false;
}

void clearPhysicalKeys() {
  memset(ps2Held, 0, sizeof(ps2Held));
  physicalHeldCount = 0;
}

void addHID(
  uint16_t u,
  bool down,
  bool media = false
) {
  if (!u) return;

  if (!keyboard.isConnected() ||
      !keyboard.isPaired()) {
    return;
  }

  uint16_t next = (hidW + 1) & HID_MASK;

  if (next == hidR) {
    ++hidDrops;
    hidNeedsRelease = true;
    return;
  }

  hidEvents[hidW] = {
    u,
    millis(),
    down,
    media
  };

  hidW = next;

  uint16_t pending = (hidW - hidR) & HID_MASK;

  if (pending > maxQueue) {
    maxQueue = pending;
  }
}

// ====================================================
// 5. SCAN CODE SET 2 -> USB HID
// ====================================================

uint8_t keyUsage(uint8_t c, bool ext) {
  if (ext) {
    switch (c) {
      case 0x11: return 0xE6;
      case 0x14: return 0xE4;
      case 0x1F: return 0xE3;
      case 0x27: return 0xE7;
      case 0x2F: return 0x65;

      case 0x4A: return 0x54;
      case 0x5A: return 0x58;

      case 0x69: return 0x4D;
      case 0x6B: return 0x50;
      case 0x6C: return 0x4A;
      case 0x70: return 0x49;
      case 0x71: return 0x4C;
      case 0x72: return 0x51;
      case 0x74: return 0x4F;
      case 0x75: return 0x52;
      case 0x7A: return 0x4E;
      case 0x7D: return 0x4B;

      case 0x7C: return 0x46;

      default: return 0;
    }
  }

  switch (c) {
    // Letras

    case 0x1C: return 0x04;
    case 0x32: return 0x05;
    case 0x21: return 0x06;
    case 0x23: return 0x07;
    case 0x24: return 0x08;
    case 0x2B: return 0x09;
    case 0x34: return 0x0A;
    case 0x33: return 0x0B;
    case 0x43: return 0x0C;
    case 0x3B: return 0x0D;
    case 0x42: return 0x0E;
    case 0x4B: return 0x0F;
    case 0x3A: return 0x10;
    case 0x31: return 0x11;
    case 0x44: return 0x12;
    case 0x4D: return 0x13;
    case 0x15: return 0x14;
    case 0x2D: return 0x15;
    case 0x1B: return 0x16;
    case 0x2C: return 0x17;
    case 0x3C: return 0x18;
    case 0x2A: return 0x19;
    case 0x1D: return 0x1A;
    case 0x22: return 0x1B;
    case 0x35: return 0x1C;
    case 0x1A: return 0x1D;

    // Numeros

    case 0x16: return 0x1E;
    case 0x1E: return 0x1F;
    case 0x26: return 0x20;
    case 0x25: return 0x21;
    case 0x2E: return 0x22;
    case 0x36: return 0x23;
    case 0x3D: return 0x24;
    case 0x3E: return 0x25;
    case 0x46: return 0x26;
    case 0x45: return 0x27;

    // Edicion

    case 0x5A: return 0x28;
    case 0x76: return 0x29;
    case 0x66: return 0x2A;
    case 0x0D: return 0x2B;
    case 0x29: return 0x2C;

    case 0x4E: return 0x2D;
    case 0x55: return 0x2E;
    case 0x54: return 0x2F;
    case 0x5B: return 0x30;
    case 0x5D: return 0x31;
    case 0x4C: return 0x33;
    case 0x52: return 0x34;
    case 0x0E: return 0x35;
    case 0x41: return 0x36;
    case 0x49: return 0x37;
    case 0x4A: return 0x38;

    case 0x61: return 0x64;
    case 0x58: return 0x39;

    // F1-F12

    case 0x05: return 0x3A;
    case 0x06: return 0x3B;
    case 0x04: return 0x3C;
    case 0x0C: return 0x3D;
    case 0x03: return 0x3E;
    case 0x0B: return 0x3F;
    case 0x83: return 0x40;
    case 0x0A: return 0x41;
    case 0x01: return 0x42;
    case 0x09: return 0x43;
    case 0x78: return 0x44;
    case 0x07: return 0x45;

    // Numpad

    case 0x77: return 0x53;
    case 0x7C: return 0x55;
    case 0x7B: return 0x56;
    case 0x79: return 0x57;

    case 0x69: return 0x59;
    case 0x72: return 0x5A;
    case 0x7A: return 0x5B;
    case 0x6B: return 0x5C;
    case 0x73: return 0x5D;
    case 0x74: return 0x5E;
    case 0x6C: return 0x5F;
    case 0x75: return 0x60;
    case 0x7D: return 0x61;
    case 0x70: return 0x62;
    case 0x71: return 0x63;

    // Modificadores

    case 0x14: return 0xE0;
    case 0x12: return 0xE1;
    case 0x11: return 0xE2;
    case 0x59: return 0xE5;

    case 0x7E: return 0x47;

    default: return 0;
  }
}

// ====================================================
// 6. MULTIMEDIA
// ====================================================

uint16_t mediaUsage(uint8_t c, bool ext) {
  if (!ext) return 0;

  switch (c) {
    case 0x4D: return MEDIA_NEXT_TRACK;
    case 0x15: return MEDIA_PREV_TRACK;
    case 0x3B: return MEDIA_STOP;
    case 0x34: return MEDIA_PLAY_PAUSE;
    case 0x50: return MEDIA_MEDIA_SELECT;

    case 0x23: return MEDIA_MUTE;
    case 0x32: return MEDIA_VOLUME_UP;
    case 0x21: return MEDIA_VOLUME_DOWN;

    case 0x48: return MEDIA_MAIL;
    case 0x2B: return MEDIA_CALCULATOR;
    case 0x40: return MEDIA_FILE_EXPLORER;

    case 0x10: return MEDIA_BROWSER_SEARCH;
    case 0x3A: return MEDIA_BROWSER_HOME;
    case 0x38: return MEDIA_BROWSER_BACK;
    case 0x30: return MEDIA_BROWSER_FORWARD;
    case 0x28: return MEDIA_BROWSER_STOP;
    case 0x20: return MEDIA_BROWSER_REFRESH;
    case 0x18: return MEDIA_BROWSER_BOOKMARKS;

    default: return 0;
  }
}

// ====================================================
// 7. DECODIFICADOR PS/2
// ====================================================

uint32_t bytesOK = 0;
uint32_t makes = 0;
uint32_t breaks = 0;
uint32_t pauses = 0;

uint32_t badStart = 0;
uint32_t badParity = 0;
uint32_t badStop = 0;
uint32_t cutFrames = 0;

uint32_t lastFallUs = 0;
uint32_t lastDecodedByteUs = 0;
uint32_t lastDropSeen = 0;

bool haveFall = false;

uint8_t bitIndex = 0;
uint8_t frameByte = 0;
uint8_t parityBits = 0;

bool prefixE0 = false;
bool prefixF0 = false;

uint8_t pauseIndex = 0;

const uint8_t PAUSE_SEQ[8] = {
  0xE1, 0x14, 0x77, 0xE1,
  0xF0, 0x14, 0xF0, 0x77
};

void resetFrame() {
  bitIndex = 0;
  frameByte = 0;
  parityBits = 0;
}

void resetParser() {
  prefixE0 = false;
  prefixF0 = false;
  pauseIndex = 0;
}

void syncAfterPS2Error() {
  resetFrame();
  resetParser();
  clearPhysicalKeys();

  hidNeedsRelease = true;
  ++resyncs;
}

void processKey(
  uint8_t c,
  bool ext,
  bool down
) {
  if (ext && c == 0x12) return;

  uint16_t media = mediaUsage(c, ext);
  uint8_t key = keyUsage(c, ext);

  if (!media && !key) {
    ++unmapped;

    if (debugKeys) {
      Serial.printf(
        "[PS2] Sin mapa: %s%02X\n",
        ext ? "E0 " : "",
        c
      );
    }

    return;
  }

  if (down) ++makes;
  else ++breaks;

  uint16_t index = c + (ext ? 256 : 0);

  if (down && !ps2Held[index]) {
    ps2Held[index] = true;
    ++physicalHeldCount;
  }
  else if (!down && ps2Held[index]) {
    ps2Held[index] = false;
    --physicalHeldCount;
  }

  lastKeyMs = millis();

  addHID(
    media ? media : key,
    down,
    media != 0
  );

  if (debugKeys) {
    Serial.printf(
      "[KEY] %s %s%02X\n",
      down ? "MAKE" : "BREAK",
      ext ? "E0 " : "",
      c
    );
  }
}

// ====================================================
// 8. CONTROL PS/2 DE LOS LED
// ====================================================

enum CmdState : uint8_t {
  CMD_IDLE,
  CMD_WAIT_ECHO,
  CMD_WAIT_ED_ACK,
  CMD_SEND_MASK,
  CMD_WAIT_MASK_ACK
};

CmdState cmdState = CMD_IDLE;

bool wantEcho = false;
bool wantLED = false;

bool echoOK = false;
bool autoLED = true;
bool txBlocked = false;

bool ledKnown = false;
bool readyPrev = false;

uint8_t ledTarget = 0;
uint8_t ledConfirmed = 0;
uint8_t lastObservedLED = 0;

uint32_t ledObservedAt = 0;
uint32_t commandStartedAt = 0;
uint32_t connectedAt = 0;

uint32_t txOK = 0;
uint32_t txErrors = 0;

uint8_t hostLEDMask() {
  uint8_t v = 0;

  if (keyboard.isScrollLockOn()) v |= 1;
  if (keyboard.isNumLockOn()) v |= 2;
  if (keyboard.isCapsLockOn()) v |= 4;

  return v;
}

void commandError(const char *why) {
  ++txErrors;

  txBlocked = true;
  autoLED = false;

  wantEcho = false;
  wantLED = false;

  cmdState = CMD_IDLE;

  Serial.printf(
    "[TX] BLOQUEADO: %s\n",
    why
  );

  Serial.println(
    "[TX] Nuevos comandos PS/2 deshabilitados."
  );
}

bool consumeCommandAnswer(uint8_t b) {
  if (
    cmdState == CMD_WAIT_ECHO &&
    b == 0xEE
  ) {
    cmdState = CMD_IDLE;
    echoOK = true;
    ++txOK;

    ledKnown = false;
    lastObservedLED = hostLEDMask();
    ledObservedAt = millis();

    Serial.println("[TX] ECHO 0xEE correcto");

    return true;
  }

  if (
    cmdState == CMD_WAIT_ED_ACK &&
    b == 0xFA
  ) {
    cmdState = CMD_SEND_MASK;
    commandStartedAt = millis();

    return true;
  }

  if (
    cmdState == CMD_WAIT_MASK_ACK &&
    b == 0xFA
  ) {
    cmdState = CMD_IDLE;

    ledConfirmed = ledTarget;
    ledKnown = true;

    ++txOK;

    Serial.printf(
      "[LED] OK 0x%02X "
      "(Caps=%d Num=%d Scroll=%d)\n",
      ledConfirmed,
      !!(ledConfirmed & 4),
      !!(ledConfirmed & 2),
      !!(ledConfirmed & 1)
    );

    return true;
  }

  if (
    b == 0xFE &&
    cmdState != CMD_IDLE
  ) {
    commandError("PS/2 solicito RESEND");
    return true;
  }

  return (
    b == 0xFA ||
    b == 0xFE ||
    b == 0xEE ||
    b == 0xAA
  );
}

void processPS2Byte(uint8_t b) {
  uint32_t now = micros();

  if (
    lastDecodedByteUs &&
    (uint32_t)(
      now - lastDecodedByteUs
    ) > 100000
  ) {
    resetParser();
  }

  lastDecodedByteUs = now;

  if (consumeCommandAnswer(b)) return;

  if (pauseIndex || b == 0xE1) {
    if (b == PAUSE_SEQ[pauseIndex]) {
      ++pauseIndex;

      if (pauseIndex == 8) {
        pauseIndex = 0;
        ++pauses;

        addHID(0x48, true);
        addHID(0x48, false);
      }
    }
    else {
      resetParser();
    }

    return;
  }

  if (b == 0xE0) {
    prefixE0 = true;
    return;
  }

  if (b == 0xF0) {
    prefixF0 = true;
    return;
  }

  bool ext = prefixE0;
  bool down = !prefixF0;

  prefixE0 = false;
  prefixF0 = false;

  processKey(b, ext, down);
}

void processFall(const PS2Edge &e) {
  if (haveFall) {
    uint32_t dt = e.timeUs - lastFallUs;

    if (dt > 300 && bitIndex != 0) {
      ++cutFrames;
      syncAfterPS2Error();
    }
  }

  lastFallUs = e.timeUs;
  haveFall = true;

  uint8_t bit = e.data & 1;

  if (bitIndex == 0) {
    if (bit) {
      ++badStart;
      return;
    }

    frameByte = 0;
    parityBits = 0;
  }
  else if (bitIndex <= 8) {
    frameByte |=
      (uint8_t)(bit << (bitIndex - 1));

    parityBits ^= bit;
  }
  else if (bitIndex == 9) {
    parityBits ^= bit;
  }

  ++bitIndex;

  if (bitIndex == 11) {
    if (!bit) {
      ++badStop;
      syncAfterPS2Error();
    }
    else if (parityBits != 1) {
      ++badParity;
      syncAfterPS2Error();
    }
    else {
      ++bytesOK;
      processPS2Byte(frameByte);
    }

    resetFrame();
  }
}

void servicePS2() {
  uint32_t nDrops;

  noInterrupts();
  nDrops = edgeDrops;
  interrupts();

  if (nDrops != lastDropSeen) {
    lastDropSeen = nDrops;

    noInterrupts();
    edgeR = edgeW;
    interrupts();

    syncAfterPS2Error();
  }

  PS2Edge e;
  uint16_t count = 0;

  while (
    count < 512 &&
    popEdge(e)
  ) {
    if (e.clock == LOW) {
      processFall(e);
    }

    ++count;
  }
}

// ====================================================
// 9. TRANSMISOR PS/2
// ====================================================

bool waitPin(
  uint8_t pin,
  uint8_t level,
  uint32_t timeoutUs
) {
  uint32_t start = micros();

  while (digitalRead(pin) != level) {
    if (
      (uint32_t)(
        micros() - start
      ) >= timeoutUs
    ) {
      return false;
    }
  }

  return true;
}

bool electricalBusIdle(uint32_t quietUs) {
  if (
    bitIndex != 0 ||
    edgeR != edgeW ||
    transmitting
  ) {
    return false;
  }

  if (
    digitalRead(PIN_CLOCK) != HIGH ||
    digitalRead(PIN_DATA) != HIGH
  ) {
    return false;
  }

  return (
    (uint32_t)(
      micros() - lastEdgeUs
    ) > quietUs
  );
}

bool readyForNewCommand() {
  return (
    electricalBusIdle(PS2_TX_QUIET_US) &&
    physicalHeldCount == 0 &&
    hidR == hidW
  );
}

bool sendPS2Byte(uint8_t b) {
  if (!electricalBusIdle(3000)) {
    return false;
  }

  transmitting = true;

  digitalWrite(PIN_CLOCK, HIGH);
  digitalWrite(PIN_DATA, HIGH);

  pinMode(
    PIN_CLOCK,
    OUTPUT_OPEN_DRAIN
  );

  pinMode(
    PIN_DATA,
    OUTPUT_OPEN_DRAIN
  );

  digitalWrite(PIN_CLOCK, HIGH);
  digitalWrite(PIN_DATA, HIGH);

  bool ok = true;

  digitalWrite(PIN_CLOCK, LOW);
  delayMicroseconds(160);

  digitalWrite(PIN_DATA, LOW);
  delayMicroseconds(15);

  digitalWrite(PIN_CLOCK, HIGH);

  if (!waitPin(PIN_CLOCK, LOW, 15000)) {
    ok = false;
  }

  uint8_t oddParity = 1;

  for (
    uint8_t i = 0;
    i < 9 && ok;
    ++i
  ) {
    uint8_t bit;

    if (i < 8) {
      bit = (b >> i) & 1;
      oddParity ^= bit;
    }
    else {
      bit = oddParity;
    }

    digitalWrite(
      PIN_DATA,
      bit ? HIGH : LOW
    );

    if (!waitPin(PIN_CLOCK, HIGH, 2000)) {
      ok = false;
      break;
    }

    if (!waitPin(PIN_CLOCK, LOW, 2000)) {
      ok = false;
      break;
    }
  }

  if (ok) {
    digitalWrite(PIN_DATA, HIGH);

    if (!waitPin(PIN_CLOCK, HIGH, 2000)) {
      ok = false;
    }

    if (
      ok &&
      !waitPin(PIN_DATA, LOW, 2000)
    ) {
      ok = false;
    }

    if (
      ok &&
      !waitPin(PIN_CLOCK, LOW, 2000)
    ) {
      ok = false;
    }

    if (
      ok &&
      !waitPin(PIN_CLOCK, HIGH, 2000)
    ) {
      ok = false;
    }

    if (
      ok &&
      !waitPin(PIN_DATA, HIGH, 2000)
    ) {
      ok = false;
    }
  }

  digitalWrite(PIN_DATA, HIGH);
  digitalWrite(PIN_CLOCK, HIGH);

  pinMode(PIN_DATA, INPUT_PULLUP);
  pinMode(PIN_CLOCK, INPUT_PULLUP);

  resetFrame();
  resetParser();
  haveFall = false;

  lastEdgeUs = micros();
  transmitting = false;

  return ok;
}

// ====================================================
// 10. CONTROL AUTOMATICO DE LED
// ====================================================

void serviceCommands() {
  if (txBlocked) return;

  uint32_t now = millis();

  if (
    cmdState == CMD_WAIT_ECHO ||
    cmdState == CMD_WAIT_ED_ACK ||
    cmdState == CMD_WAIT_MASK_ACK
  ) {
    if (
      (uint32_t)(
        now - commandStartedAt
      ) > CMD_TIMEOUT_MS
    ) {
      commandError("sin respuesta del teclado");
    }

    return;
  }

  if (cmdState == CMD_SEND_MASK) {
    if (
      (uint32_t)(
        now - commandStartedAt
      ) > LED_PARAM_TIMEOUT_MS
    ) {
      commandError(
        "no fue posible completar 0xED"
      );

      return;
    }

    if (!electricalBusIdle(3000)) return;

    if (!sendPS2Byte(ledTarget)) {
      commandError(
        "fallo envio mascara LED"
      );

      return;
    }

    cmdState = CMD_WAIT_MASK_ACK;
    commandStartedAt = millis();

    return;
  }

  if (!readyPrev) return;

  if (wantEcho) {
    if (!readyForNewCommand()) return;

    wantEcho = false;

    if (!sendPS2Byte(0xEE)) {
      commandError("fallo envio ECHO");
      return;
    }

    cmdState = CMD_WAIT_ECHO;
    commandStartedAt = millis();

    return;
  }

  if (
    !echoOK ||
    (!autoLED && !wantLED)
  ) {
    return;
  }

  uint8_t newMask = hostLEDMask();

  if (newMask != lastObservedLED) {
    lastObservedLED = newMask;
    ledObservedAt = now;
  }

  if (
    autoLED &&
    (!ledKnown || newMask != ledConfirmed) &&
    (uint32_t)(
      now - ledObservedAt
    ) >= LED_STABLE_MS
  ) {
    wantLED = true;
  }

  if (!wantLED) return;

  if (!readyForNewCommand()) return;

  wantLED = false;
  ledTarget = newMask;

  if (!sendPS2Byte(0xED)) {
    commandError("fallo envio 0xED");
    return;
  }

  cmdState = CMD_WAIT_ED_ACK;
  commandStartedAt = millis();
}

// ====================================================
// 11. CONEXION BLE
// ====================================================

void serviceConnection() {
  bool ready =
    keyboard.isConnected() &&
    keyboard.isPaired();

  if (ready != readyPrev) {
    readyPrev = ready;

    clearHIDQueue();

    if (ready) {
      keyboard.releaseAll();

      connectedAt = millis();

      echoOK = false;
      ledKnown = false;

      lastObservedLED = hostLEDMask();
      ledObservedAt = millis();

      wantEcho = false;
      wantLED = false;

      Serial.println(
        "[BLE] Lista. Preparando sincronizacion."
      );
    }
    else {
      if (cmdState != CMD_IDLE) {
        commandError(
          "desconexion durante comando PS/2"
        );
      }

      echoOK = false;
      ledKnown = false;

      wantEcho = false;
      wantLED = false;

      Serial.println("[BLE] Desconectada.");
    }
  }

  if (
    ready &&
    !txBlocked &&
    !echoOK &&
    cmdState == CMD_IDLE &&
    !wantEcho &&
    (uint32_t)(
      millis() - connectedAt
    ) >= BLE_SETTLE_MS
  ) {
    wantEcho = true;
  }
}

// ====================================================
// 12. ENVIO HID
// ====================================================

void serviceHID() {
  if (!readyPrev) return;

  if (hidNeedsRelease) {
    keyboard.releaseAll();
    clearHIDQueue();

    return;
  }

  static uint32_t lastSendMs = 0;

  if (
    (uint32_t)(
      millis() - lastSendMs
    ) < hidGapMs ||
    hidR == hidW
  ) {
    return;
  }

  HIDEvent e = hidEvents[hidR];

  hidR = (hidR + 1) & HID_MASK;

  uint32_t waitMs = millis() - e.queuedMs;

  if (waitMs > maxQueueWaitMs) {
    maxQueueWaitMs = waitMs;
  }

  sumQueueWaitMs += waitMs;
  ++queueSamples;

  if (e.media) {
    if (e.down) {
      if (mediaHeld == e.usage) {
        ++ignored;
        return;
      }

      if (mediaHeld) {
        keyboard.release(mediaHeld);
      }

      mediaHeld = e.usage;

      keyboard.press(
        (uint16_t)e.usage
      );
    }
    else {
      if (mediaHeld != e.usage) {
        ++ignored;
        return;
      }

      keyboard.release(
        (uint16_t)e.usage
      );

      mediaHeld = 0;
    }
  }
  else {
    uint8_t u = (uint8_t)e.usage;

    if (hidHeld[u] == e.down) {
      ++ignored;
      return;
    }

    hidHeld[u] = e.down;

    if (e.down) {
      keyboard.press(u);
    }
    else {
      keyboard.release(u);
    }
  }

  ++hidSent;
  lastSendMs = millis();
}

// ====================================================
// 13. NUEVA FUNCION: KEEP-ALIVE
// ====================================================

// No crea conexiones USB falsas.
// Solo intenta incrementar el consumo electrico
// del propio ESP32 durante una ventana periodica.
//
// Cada porcion de calculo dura aproximadamente
// 1.5 ms para no bloquear el loop durante segundos.
//
// NO enviar comandos PS/2 desde aqui.
// NO alterar las teclas HID.

void serviceKeepAlive() {
  if (!keepAliveEnabled) return;

  uint32_t now = millis();

  if (!keepAliveActive) {
    if (
      (uint32_t)(
        now - keepWindowStartMs
      ) >= KEEP_INTERVAL_MS
    ) {
      keepAliveActive = true;
      keepPulseStartMs = now;
      ++keepPulseCount;

      Serial.printf(
        "[KEEP] Pulso %lu iniciado\n",
        (unsigned long)keepPulseCount
      );
    }

    return;
  }

  if (
    (uint32_t)(
      now - keepPulseStartMs
    ) >= KEEP_DURATION_MS
  ) {
    keepAliveActive = false;
    keepWindowStartMs = now;

    Serial.println("[KEEP] Pulso terminado");
    return;
  }

  // Calculo que el compilador no puede
  // eliminar como operacion inutil.

  uint32_t startUs = micros();
  uint32_t value = keepCalculation;

  while (
    (uint32_t)(
      micros() - startUs
    ) < KEEP_SLICE_US
  ) {
    value = value * 1664525UL + 1013904223UL;
    value ^= (value >> 13);
  }

  keepCalculation = value;
}

// ====================================================
// 14. WATCHDOG
// ====================================================

bool watchdogEnabled = false;

void setupWatchdog() {
  esp_task_wdt_config_t cfg = {};

  cfg.timeout_ms = 15000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;

  esp_err_t rc = esp_task_wdt_init(&cfg);

  if (rc == ESP_ERR_INVALID_STATE) {
    rc = esp_task_wdt_reconfigure(&cfg);
  }

  if (rc == ESP_OK) {
    rc = esp_task_wdt_add(NULL);

    if (
      rc == ESP_OK ||
      rc == ESP_ERR_INVALID_STATE
    ) {
      watchdogEnabled = true;
    }
  }

  Serial.printf(
    "[WDT] %s (15 segundos)\n",
    watchdogEnabled ? "activo" : "no disponible"
  );
}

// ====================================================
// 15. REINICIOS
// ====================================================

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";

    default: return "OTRO";
  }
}

// ====================================================
// 16. ESTADISTICAS
// ====================================================

void printStats() {
  uint32_t dropped;

  noInterrupts();
  dropped = edgeDrops;
  interrupts();

  uint16_t pending =
    (hidW - hidR) & HID_MASK;

  uint32_t average = queueSamples
    ? (uint32_t)(
        sumQueueWaitMs / queueSamples
      )
    : 0;

  Serial.println();
  Serial.println("===== HP KB9970 V4.6.2 =====");

  Serial.printf(
    "[BLE] CONNECTED=%d PAIRED=%d READY=%d\n",
    keyboard.isConnected(),
    keyboard.isPaired(),
    readyPrev
  );

  Serial.printf(
    "[PS2] BYTES=%lu MAKE=%lu BREAK=%lu "
    "ERR_START=%lu ERR_PAR=%lu ERR_STOP=%lu "
    "CUT=%lu DROPS=%lu RESYNC=%lu\n",
    (unsigned long)bytesOK,
    (unsigned long)makes,
    (unsigned long)breaks,
    (unsigned long)badStart,
    (unsigned long)badParity,
    (unsigned long)badStop,
    (unsigned long)cutFrames,
    (unsigned long)dropped,
    (unsigned long)resyncs
  );

  Serial.printf(
    "[HID] GAP=%u SENT=%lu DROP=%lu "
    "PENDING=%u PEAK=%u "
    "AVG_WAIT=%lu MAX_WAIT=%lu HELD_PS2=%u\n",
    (unsigned)hidGapMs,
    (unsigned long)hidSent,
    (unsigned long)hidDrops,
    (unsigned)pending,
    (unsigned)maxQueue,
    (unsigned long)average,
    (unsigned long)maxQueueWaitMs,
    (unsigned)physicalHeldCount
  );

  Serial.printf(
    "[LED] HOST=%02X CONFIRMED=%02X "
    "KNOWN=%d AUTO=%d ECHO=%d "
    "TX_BLOCK=%d OK=%lu ERROR=%lu STATE=%u\n",
    hostLEDMask(),
    ledConfirmed,
    ledKnown,
    autoLED,
    echoOK,
    txBlocked,
    (unsigned long)txOK,
    (unsigned long)txErrors,
    (unsigned)cmdState
  );

  Serial.printf(
    "[KEEP] ENABLED=%d ACTIVE=%d "
    "PULSES=%lu INTERVAL=%lu ms "
    "DURATION=%lu ms\n",
    keepAliveEnabled,
    keepAliveActive,
    (unsigned long)keepPulseCount,
    (unsigned long)KEEP_INTERVAL_MS,
    (unsigned long)KEEP_DURATION_MS
  );

  Serial.printf(
    "[SYS] FREE_HEAP=%lu MIN_HEAP=%lu "
    "UPTIME=%lu s WDT=%d\n",
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMinFreeHeap(),
    (unsigned long)(millis() / 1000),
    watchdogEnabled
  );
}

// ====================================================
// 17. COMANDOS SERIE
// ====================================================

void printHelp() {
  Serial.println();
  Serial.println("=== COMANDOS ===");
  Serial.println("s = estadisticas");
  Serial.println("d = debug PS/2");
  Serial.println("h = ayuda");
  Serial.println("1 = HID 20 ms");
  Serial.println("2 = HID 12 ms");
  Serial.println("3 = HID 8 ms");
  Serial.println("e = ECHO PS/2");
  Serial.println("l = sincronizar LED");
  Serial.println("a = alternar LED automatico");
  Serial.println("b = reiniciar Bluetooth");
  Serial.println("r = reiniciar ESP32");

  Serial.println("k = alternar KEEP-ALIVE");
  Serial.println("p = provocar un pulso KEEP-ALIVE");

  Serial.println();
}

void serialCommands() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == 's') {
      printStats();
    }
    else if (c == 'h') {
      printHelp();
    }
    else if (c == 'd') {
      debugKeys = !debugKeys;

      Serial.printf(
        "[DEBUG] %d\n",
        debugKeys
      );
    }
    else if (c >= '1' && c <= '3') {
      hidGapMs =
        c == '1' ? 20 :
        c == '2' ? 12 : 8;

      Serial.printf(
        "[HID] GAP=%u ms\n",
        (unsigned)hidGapMs
      );
    }
    else if (c == 'k') {
      keepAliveEnabled = !keepAliveEnabled;

      if (!keepAliveEnabled) {
        keepAliveActive = false;
      }

      keepWindowStartMs = millis();

      Serial.printf(
        "[KEEP] ENABLED=%d\n",
        keepAliveEnabled
      );
    }
    else if (c == 'p') {
      keepAliveEnabled = true;
      keepAliveActive = true;
      keepPulseStartMs = millis();

      ++keepPulseCount;

      Serial.println(
        "[KEEP] Pulso manual iniciado"
      );
    }
    else if (
      c == 'e' &&
      !txBlocked &&
      cmdState == CMD_IDLE
    ) {
      wantEcho = true;

      Serial.println(
        "[TX] ECHO solicitado"
      );
    }
    else if (
      c == 'l' &&
      !txBlocked &&
      echoOK &&
      cmdState == CMD_IDLE
    ) {
      wantLED = true;

      Serial.println(
        "[LED] Sincronizacion solicitada"
      );
    }
    else if (
      c == 'a' &&
      !txBlocked
    ) {
      autoLED = !autoLED;

      Serial.printf(
        "[LED] Automatico=%d\n",
        autoLED
      );
    }
    else if (c == 'b') {
      if (cmdState != CMD_IDLE) {
        Serial.println(
          "[BLE] Esperar a terminar PS/2"
        );
      }
      else {
        Serial.println(
          "[BLE] Reinicio manual"
        );

        keyboard.end();
        delay(150);
        keyboard.begin();
      }
    }
    else if (c == 'r') {
      Serial.println(
        "[SYS] Reinicio manual"
      );

      Serial.flush();
      ESP.restart();
    }
  }
}

// ====================================================
// 18. SETUP
// ====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  esp_reset_reason_t cause =
    esp_reset_reason();

  Serial.printf(
    "\nHP KB9970 V4.6.2 - REINICIO: %s (%d)\n",
    resetName(cause),
    (int)cause
  );

  pinMode(PIN_DATA, INPUT_PULLUP);
  pinMode(PIN_CLOCK, INPUT_PULLUP);

  setupWatchdog();

  if (
    cause == ESP_RST_TASK_WDT ||
    cause == ESP_RST_INT_WDT ||
    cause == ESP_RST_WDT ||
    cause == ESP_RST_PANIC ||
    cause == ESP_RST_BROWNOUT
  ) {
    autoLED = false;
    txBlocked = true;

    Serial.println(
      "[SAFE] TX PS/2 bloqueada por reinicio anormal."
    );
  }

  keyboard.setLogLevel(
    HIDLogLevel::Normal
  );

  keyboard.setSecurityMode(
    HIDSecurity::JustWorks
  );

  keyboard.setConnIntervalMs(30);
  keyboard.onPairingComplete(pairingDone);
  keyboard.begin();

  attachInterrupt(
    digitalPinToInterrupt(PIN_CLOCK),
    clockISR,
    CHANGE
  );

  // Programar primer pulso.
  keepWindowStartMs = millis();

  Serial.println(
    "[CONFIG] HID 8 ms - KEEP-ALIVE ACTIVO"
  );

  printHelp();
}

// ====================================================
// 19. LOOP PRINCIPAL
// ====================================================

void loop() {
  // Atender primero la entrada fisica.
  servicePS2();

  // Mantener conexion BLE.
  serviceConnection();

  // Enviar eventos HID.
  serviceHID();

  // Controlar luces originales.
  serviceCommands();

  // Gestionar comandos de diagnostico.
  serialCommands();

  // Actividad periodica para intentar mantener
  // encendida la salida de 5 V del power bank.
  serviceKeepAlive();

  if (pairingStatus != -1) {
    int8_t result = pairingStatus;
    pairingStatus = -1;

    Serial.printf(
      "[BLE] PAIRING=%s\n",
      result ? "OK" : "ERROR"
    );
  }

  if (watchdogEnabled) {
    esp_task_wdt_reset();
  }

  delay(1);
}
