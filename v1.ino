
#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>

// ========================================================
// HP KB-9970 BLUETOOTH V4.4
// ESP32-C3 SuperMini
//
// PS/2 DATA  -> GPIO4
// PS/2 CLOCK -> GPIO3
//
// Biblioteca HijelHID_BLEKeyboard 0.6.x
// NimBLE-Arduino 2.x
//
// NOVEDADES:
// - ECHO PS/2 automatico
// - Sincronizacion LED automatica
// - Recuperacion de estados tras reconexion BLE
// - Sin necesidad de comandos e, l, a al encender
// - Bloqueo de TX ante fallos
//
// IMPORTANTE:
// El teclado HP funciona experimentalmente
// a 3.3 V, pero su alimentacion nominal es 5 V.
// No conectar 5 V directamente a GPIO del ESP32.
// ========================================================

constexpr uint8_t DATA_PIN = 4;
constexpr uint8_t CLOCK_PIN = 3;

constexpr uint32_t AUTO_START_DELAY_MS = 1500;
constexpr uint32_t AUTO_LED_DELAY_MS = 350;

// ========================================================
// 1. TIPOS Y PROTOTIPOS
// ========================================================

// Declaramos los tipos antes de los prototipos
// para evitar el error de Arduino IDE.

struct PS2Edge {
  uint32_t us;
  uint8_t clk;
  uint8_t data;
};

struct HIDEvent {
  uint16_t usage;
  bool down;
  bool media;
};

bool popEdge(PS2Edge &e);
void fallingEdge(const PS2Edge &e);
void receiveByte(uint8_t b);

void handleKey(
  uint8_t code,
  bool ext,
  bool down
);

uint8_t keyboardUsage(
  uint8_t code,
  bool ext
);

uint16_t consumerUsage(
  uint8_t code,
  bool ext
);

void drainPS2();
void serviceHID();
void servicePS2Commands();
void serviceAutoStartup();
void printStats();
void resetParser();
void resetFrame();

bool sendHostByte(uint8_t b);
bool busQuiet();

void commandFailure(const char *reason);
void resetLEDSync();
uint8_t readHostLEDs();

// ========================================================
// 2. BLUETOOTH
// ========================================================

// Mantener el mismo nombre para conservar
// el emparejamiento que ya funciona.

HijelHID_BLEKeyboard keyboard(
  "HP KB9970 V3",
  "KB9970 DIY",
  100
);

// El 100 es un nivel de bateria simulado.

volatile int8_t pairingResult = -1;

void paired(bool success) {
  pairingResult = success ? 1 : 0;
}

// ========================================================
// 3. CAPTURA PS/2 POR AMBOS FLANCOS
// ========================================================

constexpr uint16_t EDGE_SIZE = 1024;
constexpr uint16_t EDGE_MASK = EDGE_SIZE - 1;

volatile PS2Edge edgeQ[EDGE_SIZE];

volatile uint16_t edgeW = 0;
volatile uint16_t edgeR = 0;

volatile uint32_t edgeDrops = 0;
volatile uint32_t lastEdgeIRQ = 0;

volatile bool hostTransmitting = false;

void ARDUINO_ISR_ATTR onClockEdge() {
  if (hostTransmitting) return;

  uint32_t now = micros();
  lastEdgeIRQ = now;

  uint16_t next = (edgeW + 1) & EDGE_MASK;

  if (next == edgeR) {
    ++edgeDrops;
    return;
  }

  edgeQ[edgeW].us = now;

  edgeQ[edgeW].clk =
    digitalRead(CLOCK_PIN);

  edgeQ[edgeW].data =
    digitalRead(DATA_PIN);

  edgeW = next;
}

bool popEdge(PS2Edge &e) {
  noInterrupts();

  if (edgeR == edgeW) {
    interrupts();
    return false;
  }

  uint16_t i = edgeR;

  e.us = edgeQ[i].us;
  e.clk = edgeQ[i].clk;
  e.data = edgeQ[i].data;

  edgeR = (i + 1) & EDGE_MASK;

  interrupts();

  return true;
}

// ========================================================
// 4. COLA DE EVENTOS HID
// ========================================================

constexpr uint16_t HID_SIZE = 256;
constexpr uint16_t HID_MASK = HID_SIZE - 1;

HIDEvent hidQ[HID_SIZE];

uint16_t hidW = 0;
uint16_t hidR = 0;

bool held[256] = {false};

uint16_t activeMedia = 0;

bool needResync = false;

uint32_t hidDrops = 0;
uint32_t hidSent = 0;
uint32_t hidIgnored = 0;
uint32_t unmapped = 0;

bool debugKeys = false;

void resetHID() {
  memset(held, 0, sizeof(held));

  activeMedia = 0;
  hidR = hidW;
  needResync = false;
}

void enqueueKey(
  uint16_t usage,
  bool down,
  bool media = false
) {
  if (usage == 0) return;

  if (!keyboard.isPaired()) return;

  uint16_t next = (hidW + 1) & HID_MASK;

  if (next == hidR) {
    ++hidDrops;
    needResync = true;
    return;
  }

  hidQ[hidW].usage = usage;
  hidQ[hidW].down = down;
  hidQ[hidW].media = media;

  hidW = next;
}

// ========================================================
// 5. PS/2 SCAN CODE SET 2 -> USB HID
// ========================================================

uint8_t keyboardUsage(uint8_t c, bool ext) {

  if (ext) {
    switch (c) {

      // Modificadores derechos
      case 0x11: return 0xE6; // Right Alt
      case 0x14: return 0xE4; // Right Ctrl

      // Windows y Menu
      case 0x1F: return 0xE3; // Left GUI
      case 0x27: return 0xE7; // Right GUI
      case 0x2F: return 0x65; // Menu

      // Teclado numerico
      case 0x4A: return 0x54; // KP /
      case 0x5A: return 0x58; // KP Enter

      // Navegacion
      case 0x69: return 0x4D; // End
      case 0x6B: return 0x50; // Left
      case 0x6C: return 0x4A; // Home
      case 0x70: return 0x49; // Insert
      case 0x71: return 0x4C; // Delete
      case 0x72: return 0x51; // Down
      case 0x74: return 0x4F; // Right
      case 0x75: return 0x52; // Up
      case 0x7A: return 0x4E; // Page Down
      case 0x7D: return 0x4B; // Page Up

      // Print Screen
      case 0x7C: return 0x46;

      default: return 0;
    }
  }

  switch (c) {

    // ---------------- LETRAS ----------------

    case 0x1C: return 0x04; // A
    case 0x32: return 0x05; // B
    case 0x21: return 0x06; // C
    case 0x23: return 0x07; // D
    case 0x24: return 0x08; // E
    case 0x2B: return 0x09; // F
    case 0x34: return 0x0A; // G
    case 0x33: return 0x0B; // H
    case 0x43: return 0x0C; // I
    case 0x3B: return 0x0D; // J
    case 0x42: return 0x0E; // K
    case 0x4B: return 0x0F; // L
    case 0x3A: return 0x10; // M
    case 0x31: return 0x11; // N
    case 0x44: return 0x12; // O
    case 0x4D: return 0x13; // P
    case 0x15: return 0x14; // Q
    case 0x2D: return 0x15; // R
    case 0x1B: return 0x16; // S
    case 0x2C: return 0x17; // T
    case 0x3C: return 0x18; // U
    case 0x2A: return 0x19; // V
    case 0x1D: return 0x1A; // W
    case 0x22: return 0x1B; // X
    case 0x35: return 0x1C; // Y
    case 0x1A: return 0x1D; // Z

    // ---------------- NUMEROS ----------------

    case 0x16: return 0x1E; // 1
    case 0x1E: return 0x1F; // 2
    case 0x26: return 0x20; // 3
    case 0x25: return 0x21; // 4
    case 0x2E: return 0x22; // 5
    case 0x36: return 0x23; // 6
    case 0x3D: return 0x24; // 7
    case 0x3E: return 0x25; // 8
    case 0x46: return 0x26; // 9
    case 0x45: return 0x27; // 0

    // ---------------- EDICION ----------------

    case 0x5A: return 0x28; // Enter
    case 0x76: return 0x29; // Escape
    case 0x66: return 0x2A; // Backspace
    case 0x0D: return 0x2B; // Tab
    case 0x29: return 0x2C; // Space

    // ---------------- SIGNOS ----------------

    case 0x4E: return 0x2D; // -
    case 0x55: return 0x2E; // =
    case 0x54: return 0x2F; // [
    case 0x5B: return 0x30; // ]
    case 0x5D: return 0x31; // Backslash
    case 0x4C: return 0x33; // ;
    case 0x52: return 0x34; // '
    case 0x0E: return 0x35; // `
    case 0x41: return 0x36; // ,
    case 0x49: return 0x37; // .
    case 0x4A: return 0x38; // /

    // Tecla adicional ISO
    case 0x61: return 0x64;

    // Caps Lock
    case 0x58: return 0x39;

    // ---------------- F1 - F12 ----------------

    case 0x05: return 0x3A; // F1
    case 0x06: return 0x3B; // F2
    case 0x04: return 0x3C; // F3
    case 0x0C: return 0x3D; // F4
    case 0x03: return 0x3E; // F5
    case 0x0B: return 0x3F; // F6
    case 0x83: return 0x40; // F7
    case 0x0A: return 0x41; // F8
    case 0x01: return 0x42; // F9
    case 0x09: return 0x43; // F10
    case 0x78: return 0x44; // F11
    case 0x07: return 0x45; // F12

    // ---------------- NUMPAD ----------------

    case 0x77: return 0x53; // Num Lock
    case 0x7C: return 0x55; // *
    case 0x7B: return 0x56; // -
    case 0x79: return 0x57; // +

    case 0x69: return 0x59; // KP 1
    case 0x72: return 0x5A; // KP 2
    case 0x7A: return 0x5B; // KP 3
    case 0x6B: return 0x5C; // KP 4
    case 0x73: return 0x5D; // KP 5
    case 0x74: return 0x5E; // KP 6
    case 0x6C: return 0x5F; // KP 7
    case 0x75: return 0x60; // KP 8
    case 0x7D: return 0x61; // KP 9
    case 0x70: return 0x62; // KP 0
    case 0x71: return 0x63; // KP .

    // ---------------- MODIFICADORES ----------------

    case 0x14: return 0xE0; // Left Ctrl
    case 0x12: return 0xE1; // Left Shift
    case 0x11: return 0xE2; // Left Alt
    case 0x59: return 0xE5; // Right Shift

    case 0x7E: return 0x47; // Scroll Lock

    default: return 0;
  }
}

// ========================================================
// 6. MULTIMEDIA
// ========================================================

uint16_t consumerUsage(uint8_t c, bool ext) {

  if (!ext) return 0;

  switch (c) {

    // Multimedia
    case 0x4D: return MEDIA_NEXT_TRACK;
    case 0x15: return MEDIA_PREV_TRACK;
    case 0x3B: return MEDIA_STOP;
    case 0x34: return MEDIA_PLAY_PAUSE;
    case 0x50: return MEDIA_MEDIA_SELECT;

    // Volumen
    case 0x23: return MEDIA_MUTE;
    case 0x32: return MEDIA_VOLUME_UP;
    case 0x21: return MEDIA_VOLUME_DOWN;

    // Aplicaciones
    case 0x48: return MEDIA_MAIL;
    case 0x2B: return MEDIA_CALCULATOR;
    case 0x40: return MEDIA_FILE_EXPLORER;

    // Navegador
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

// ========================================================
// 7. ESTADISTICAS Y PARSER PS/2
// ========================================================

uint32_t validBytes = 0;
uint32_t badStart = 0;
uint32_t badParity = 0;
uint32_t badStop = 0;
uint32_t cutFrames = 0;

uint32_t falls = 0;
uint32_t rises = 0;
uint32_t shortClocks = 0;

uint32_t makes = 0;
uint32_t breaks = 0;
uint32_t pauses = 0;

uint32_t lastFall = 0;
bool haveFall = false;

uint8_t bitIndex = 0;
uint8_t byteIn = 0;
uint8_t parityIn = 0;

bool prefixE0 = false;
bool prefixF0 = false;

uint8_t pauseIndex = 0;

const uint8_t PAUSE_SEQ[8] = {
  0xE1, 0x14, 0x77, 0xE1,
  0xF0, 0x14, 0xF0, 0x77
};

void resetFrame() {
  bitIndex = 0;
  byteIn = 0;
  parityIn = 0;
}

void resetParser() {
  prefixE0 = false;
  prefixF0 = false;
  pauseIndex = 0;
}

void handleKey(
  uint8_t code,
  bool ext,
  bool down
) {

  // Print Screen incluye un Shift ficticio.
  if (ext && code == 0x12) return;

  if (down) ++makes;
  else ++breaks;

  uint16_t media = consumerUsage(code, ext);

  if (media) {
    enqueueKey(media, down, true);
    return;
  }

  uint8_t usage = keyboardUsage(code, ext);

  if (!usage) {
    ++unmapped;

    Serial.printf(
      "[PS2] Sin mapa: %s%02X\n",
      ext ? "E0 " : "",
      code
    );

    return;
  }

  enqueueKey(usage, down, false);

  if (debugKeys) {
    Serial.printf(
      "[KEY] %s %s%02X -> %02X\n",
      down ? "MAKE" : "BREAK",
      ext ? "E0 " : "",
      code,
      usage
    );
  }
}

// ========================================================
// 8. COMANDOS PS/2 Y ESTADO DE LOS LED
// ========================================================

enum TxState : uint8_t {
  TX_IDLE,
  TX_WAIT_ECHO,
  TX_WAIT_ED_ACK,
  TX_READY_LED_VALUE,
  TX_WAIT_LED_ACK
};

TxState txState = TX_IDLE;

bool echoRequested = false;
bool ledRequested = false;

bool echoPassed = false;
bool ledPassed = false;

// AHORA ACTIVADO POR DEFECTO
bool autoLED = true;

bool txBlocked = false;

uint8_t requestedLED = 0;
uint8_t confirmedLED = 0xFF;

uint32_t txStartMs = 0;
uint32_t commandOK = 0;
uint32_t commandErrors = 0;

uint32_t lastLEDChangeMs = 0;

// Nuevo: inicio automatico.
bool startupPending = false;
uint32_t startupTime = 0;

// Los estados internos quedan separados del
// bloqueo de seguridad. No borramos txBlocked
// al producirse una simple reconexion BLE.

void resetLEDSync() {

  echoRequested = false;
  ledRequested = false;

  echoPassed = false;
  ledPassed = false;

  confirmedLED = 0xFF;

  startupPending = false;

  txState = TX_IDLE;
}

// Bits PS/2:
//
// Scroll Lock = bit 0
// Num Lock    = bit 1
// Caps Lock   = bit 2

uint8_t readHostLEDs() {

  uint8_t mask = 0;

  if (keyboard.isScrollLockOn()) {
    mask |= 0x01;
  }

  if (keyboard.isNumLockOn()) {
    mask |= 0x02;
  }

  if (keyboard.isCapsLockOn()) {
    mask |= 0x04;
  }

  return mask;
}

void commandFailure(const char *reason) {

  ++commandErrors;

  txBlocked = true;
  autoLED = false;

  echoRequested = false;
  ledRequested = false;
  startupPending = false;

  txState = TX_IDLE;

  Serial.printf(
    "[TX] ERROR: %s\n",
    reason
  );

  Serial.println(
    "[TX] Transmision PS/2 bloqueada."
  );

  Serial.println(
    "[TX] No se intentaran nuevos comandos."
  );
}

// ========================================================
// 9. RESPUESTAS DEL TECLADO
// ========================================================

bool consumeCommandResponse(uint8_t b) {

  // -----------------------------------------------
  // ECHO: comando EE, respuesta EE
  // -----------------------------------------------

  if (txState == TX_WAIT_ECHO && b == 0xEE) {

    txState = TX_IDLE;
    echoPassed = true;

    ++commandOK;

    Serial.println(
      "[AUTO] ECHO 0xEE correcto."
    );

    // NOVEDAD:
    // Ya no hace falta escribir 'l'.
    // Preparamos la sincronizacion inicial.
    if (autoLED && !txBlocked) {

      requestedLED = readHostLEDs();
      ledRequested = true;

      Serial.printf(
        "[AUTO] Sincronizacion inicial LED: 0x%02X\n",
        requestedLED
      );
    }

    return true;
  }

  // -----------------------------------------------
  // ACK despues del comando ED
  // -----------------------------------------------

  if (
    txState == TX_WAIT_ED_ACK &&
    b == 0xFA
  ) {

    txState = TX_READY_LED_VALUE;
    txStartMs = millis();

    Serial.println(
      "[TX] ACK del comando 0xED."
    );

    return true;
  }

  // -----------------------------------------------
  // ACK despues del byte LED
  // -----------------------------------------------

  if (
    txState == TX_WAIT_LED_ACK &&
    b == 0xFA
  ) {

    txState = TX_IDLE;

    ledPassed = true;
    confirmedLED = requestedLED;

    ++commandOK;

    Serial.printf(
      "[LED] OK 0x%02X "
      "Caps=%d Num=%d Scroll=%d\n",
      confirmedLED,
      !!(confirmedLED & 0x04),
      !!(confirmedLED & 0x02),
      !!(confirmedLED & 0x01)
    );

    return true;
  }

  // -----------------------------------------------
  // RESEND
  // -----------------------------------------------

  if (b == 0xFE && txState != TX_IDLE) {

    commandFailure(
      "El teclado solicito RESEND 0xFE"
    );

    return true;
  }

  // Evitar interpretar como teclas las respuestas
  // de control PS/2.

  if (
    b == 0xFA ||
    b == 0xFE ||
    b == 0xEE ||
    b == 0xAA
  ) {

    Serial.printf(
      "[PS2] Respuesta: 0x%02X\n",
      b
    );

    return true;
  }

  return false;
}

void receiveByte(uint8_t b) {

  if (consumeCommandResponse(b)) {
    return;
  }

  // Pause: secuencia especial.
  if (pauseIndex || b == 0xE1) {

    if (b == PAUSE_SEQ[pauseIndex]) {

      ++pauseIndex;

      if (pauseIndex == 8) {

        pauseIndex = 0;
        ++pauses;

        enqueueKey(0x48, true);
        enqueueKey(0x48, false);
      }
    }
    else {
      resetParser();
    }

    return;
  }

  // Prefijo extendido
  if (b == 0xE0) {
    prefixE0 = true;
    return;
  }

  // Prefijo de liberacion
  if (b == 0xF0) {
    prefixF0 = true;
    return;
  }

  bool ext = prefixE0;
  bool down = !prefixF0;

  prefixE0 = false;
  prefixF0 = false;

  handleKey(b, ext, down);
}

// ========================================================
// 10. DECODIFICACION DE TRAMAS PS/2
// ========================================================

void fallingEdge(const PS2Edge &e) {

  ++falls;

  if (haveFall) {

    uint32_t dt = e.us - lastFall;

    if (dt < 65) {
      ++shortClocks;
    }

    if (dt > 300 && bitIndex != 0) {

      ++cutFrames;

      resetFrame();
      resetParser();
    }
  }

  lastFall = e.us;
  haveFall = true;

  uint8_t bit = e.data & 1;

  if (bitIndex == 0) {

    if (bit != 0) {
      ++badStart;
      return;
    }

    byteIn = 0;
    parityIn = 0;
  }
  else if (bitIndex <= 8) {

    byteIn |= (uint8_t)(
      bit << (bitIndex - 1)
    );

    parityIn ^= bit;
  }
  else if (bitIndex == 9) {

    parityIn ^= bit;
  }

  ++bitIndex;

  if (bitIndex == 11) {

    if (bit != 1) {

      ++badStop;
      resetParser();
    }
    else if (parityIn != 1) {

      ++badParity;
      resetParser();
    }
    else {

      ++validBytes;
      receiveByte(byteIn);
    }

    resetFrame();
  }
}

void drainPS2() {

  PS2Edge e;
  uint16_t processed = 0;

  while (
    processed < 512 &&
    popEdge(e)
  ) {

    if (e.clk == 0) {
      fallingEdge(e);
    }
    else {
      ++rises;
    }

    ++processed;
  }
}

// ========================================================
// 11. TRANSMISOR PS/2
// ========================================================

// LOW  = conducir la linea a tierra.
// HIGH = soltarla mediante drenador abierto.
//
// Esta es la rutina que funciono en V4.3.
// Conservamos su secuencia de transmision.

bool waitPin(
  uint8_t pin,
  uint8_t level,
  uint32_t timeoutUs
) {

  uint32_t start = micros();

  while (
    digitalRead(pin) != level
  ) {

    if ((uint32_t)(
      micros() - start
    ) >= timeoutUs) {

      return false;
    }
  }

  return true;
}

bool busQuiet() {

  if (bitIndex != 0) return false;

  if (edgeR != edgeW) return false;

  if (hidR != hidW) return false;

  if (
    digitalRead(CLOCK_PIN) != HIGH ||
    digitalRead(DATA_PIN) != HIGH
  ) {

    return false;
  }

  return (
    (uint32_t)(
      micros() - lastEdgeIRQ
    ) > 5000
  );
}

bool sendHostByte(uint8_t b) {

  if (!busQuiet()) return false;

  hostTransmitting = true;

  // Soltar las lineas.
  digitalWrite(CLOCK_PIN, HIGH);
  digitalWrite(DATA_PIN, HIGH);

  pinMode(
    CLOCK_PIN,
    OUTPUT_OPEN_DRAIN
  );

  pinMode(
    DATA_PIN,
    OUTPUT_OPEN_DRAIN
  );

  digitalWrite(CLOCK_PIN, HIGH);
  digitalWrite(DATA_PIN, HIGH);

  bool ok = true;

  // -----------------------------------------------
  // HOST INHIBIT
  // -----------------------------------------------

  digitalWrite(CLOCK_PIN, LOW);
  delayMicroseconds(160);

  // -----------------------------------------------
  // REQUEST TO SEND
  // -----------------------------------------------

  digitalWrite(DATA_PIN, LOW);
  delayMicroseconds(15);

  digitalWrite(CLOCK_PIN, HIGH);

  // El teclado genera el CLOCK.
  if (!waitPin(CLOCK_PIN, LOW, 15000)) {
    ok = false;
  }

  // -----------------------------------------------
  // 8 DATA BITS + ODD PARITY
  // -----------------------------------------------

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
      DATA_PIN,
      bit ? HIGH : LOW
    );

    if (!waitPin(CLOCK_PIN, HIGH, 2000)) {
      ok = false;
      break;
    }

    if (!waitPin(CLOCK_PIN, LOW, 2000)) {
      ok = false;
      break;
    }
  }

  // -----------------------------------------------
  // STOP + ACK ELECTRICO
  // -----------------------------------------------

  if (ok) {

    digitalWrite(DATA_PIN, HIGH);

    if (!waitPin(CLOCK_PIN, HIGH, 2000)) {
      ok = false;
    }

    if (
      ok &&
      !waitPin(DATA_PIN, LOW, 2000)
    ) {

      ok = false;
    }

    if (
      ok &&
      !waitPin(CLOCK_PIN, LOW, 2000)
    ) {

      ok = false;
    }

    if (
      ok &&
      !waitPin(CLOCK_PIN, HIGH, 2000)
    ) {

      ok = false;
    }

    if (
      ok &&
      !waitPin(DATA_PIN, HIGH, 2000)
    ) {

      ok = false;
    }
  }

  // -----------------------------------------------
  // LIBERAR SIEMPRE EL BUS
  // -----------------------------------------------

  digitalWrite(DATA_PIN, HIGH);
  digitalWrite(CLOCK_PIN, HIGH);

  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(CLOCK_PIN, INPUT_PULLUP);

  resetFrame();
  resetParser();

  haveFall = false;
  lastEdgeIRQ = micros();

  hostTransmitting = false;

  return ok;
}

// ========================================================
// 12. ARRANQUE AUTOMATICO
// ========================================================

// El arranque se programa al establecerse
// la conexion BLE, no simplemente al encender
// el ESP32.
//
// Esto permite sincronizar de nuevo si cambiamos
// de computadora o se pierde el enlace.

void serviceAutoStartup() {

  static bool previousReady = false;

  bool ready =
    keyboard.isConnected() &&
    keyboard.isPaired();

  // -----------------------------------------------
  // NUEVA CONEXION
  // -----------------------------------------------

  if (ready && !previousReady) {

    Serial.println(
      "[AUTO] Bluetooth listo."
    );

    if (!txBlocked) {

      resetLEDSync();

      startupPending = true;
      startupTime = millis();

      // Nuevo inicio con luces habilitadas.
      autoLED = true;

      Serial.println(
        "[AUTO] Preparando sincronizacion PS/2."
      );
    }
  }

  // -----------------------------------------------
  // DESCONEXION
  // -----------------------------------------------

  if (!ready && previousReady) {

    Serial.println(
      "[AUTO] Bluetooth desconectado."
    );

    // No se debe seguir transmitiendo comandos
    // automaticos cuando no existe enlace.
    autoLED = false;
    startupPending = false;

    echoRequested = false;
    ledRequested = false;

    // Si habia una orden pendiente, no
    // asumir que fue completada.
    txState = TX_IDLE;

    echoPassed = false;
    ledPassed = false;
    confirmedLED = 0xFF;
  }

  previousReady = ready;

  if (!ready) return;
  if (txBlocked) return;
  if (!startupPending) return;

  // Esperar un tiempo despues de conectar BLE.
  if ((uint32_t)(
    millis() - startupTime
  ) < AUTO_START_DELAY_MS) {

    return;
  }

  // No iniciar diagnostico con datos pendientes.
  if (txState != TX_IDLE) return;

  startupPending = false;

  echoRequested = true;

  Serial.println(
    "[AUTO] Iniciando diagnostico ECHO 0xEE."
  );
}

// ========================================================
// 13. MAQUINA DE ESTADOS DE COMANDOS
// ========================================================

void servicePS2Commands() {

  if (txBlocked) return;

  // -----------------------------------------------
  // RESPUESTA PENDIENTE
  // -----------------------------------------------

  if (
    txState == TX_WAIT_ECHO ||
    txState == TX_WAIT_ED_ACK ||
    txState == TX_WAIT_LED_ACK
  ) {

    if ((uint32_t)(
      millis() - txStartMs
    ) > 100) {

      commandFailure(
        "Respuesta PS/2 fuera de plazo"
      );
    }

    return;
  }

  // -----------------------------------------------
  // SEGUNDO BYTE DEL COMANDO LED
  // -----------------------------------------------

  if (txState == TX_READY_LED_VALUE) {

    if ((uint32_t)(
      millis() - txStartMs
    ) > 250) {

      commandFailure(
        "Bus ocupado al enviar byte LED"
      );

      return;
    }

    if (!busQuiet()) return;

    if (!sendHostByte(requestedLED)) {

      commandFailure(
        "Error enviando mascara LED"
      );

      return;
    }

    txState = TX_WAIT_LED_ACK;
    txStartMs = millis();

    Serial.printf(
      "[TX] Mascara LED 0x%02X enviada.\n",
      requestedLED
    );

    return;
  }

  // -----------------------------------------------
  // VERIFICAR BLUETOOTH
  // -----------------------------------------------

  if (
    !keyboard.isConnected() ||
    !keyboard.isPaired()
  ) {

    return;
  }

  if (!busQuiet()) return;

  // -----------------------------------------------
  // ECHO AUTOMATICO
  // -----------------------------------------------

  if (echoRequested) {

    echoRequested = false;

    if (!sendHostByte(0xEE)) {

      commandFailure(
        "Fallo enviando ECHO 0xEE"
      );

      return;
    }

    txState = TX_WAIT_ECHO;
    txStartMs = millis();

    Serial.println(
      "[TX] ECHO enviado. Esperando 0xEE..."
    );

    return;
  }

  // Hasta que el ECHO no sea correcto,
  // no enviamos comandos de LED.

  if (!echoPassed) return;

  // -----------------------------------------------
  // SINCRONIZACION AUTOMATICA
  // -----------------------------------------------

  if (
    autoLED &&
    ledPassed &&
    (uint32_t)(
      millis() - lastLEDChangeMs
    ) > AUTO_LED_DELAY_MS
  ) {

    uint8_t desired = readHostLEDs();

    if (desired != confirmedLED) {

      requestedLED = desired;
      ledRequested = true;

      lastLEDChangeMs = millis();
    }
  }

  // -----------------------------------------------
  // ENVIAR COMANDO LED
  // -----------------------------------------------

  if (!ledRequested) return;

  ledRequested = false;

  // Actualizar el estado del host justo antes
  // de enviar el primer comando.
  requestedLED = readHostLEDs();

  if (!sendHostByte(0xED)) {

    commandFailure(
      "Fallo enviando comando LED 0xED"
    );

    return;
  }

  txState = TX_WAIT_ED_ACK;
  txStartMs = millis();

  Serial.printf(
    "[TX] 0xED enviado. Objetivo=0x%02X\n",
    requestedLED
  );
}

// ========================================================
// 14. SERVICIO BLUETOOTH HID
// ========================================================

void serviceHID() {

  static bool wasPaired = false;
  static uint32_t lastSend = 0;

  bool ready = keyboard.isPaired();

  if (ready != wasPaired) {

    wasPaired = ready;

    resetHID();

    if (ready) {

      keyboard.releaseAll();

      Serial.println(
        "[BLE] Emparejado y listo."
      );
    }
    else {

      Serial.println(
        "[BLE] Enlace no listo."
      );
    }

    lastSend = millis();
  }

  if (!ready) return;

  // Recuperacion ante desbordamiento.
  if (needResync) {

    keyboard.releaseAll();
    resetHID();

    lastSend = millis();
    return;
  }

  // Ritmo conservador de la V4.3.
  if ((uint32_t)(
    millis() - lastSend
  ) < 20) {

    return;
  }

  if (hidR == hidW) return;

  HIDEvent e = hidQ[hidR];
  hidR = (hidR + 1) & HID_MASK;

  if (e.media) {

    if (e.down) {

      if (activeMedia == e.usage) {

        ++hidIgnored;
        return;
      }

      if (activeMedia) {
        keyboard.release(activeMedia);
      }

      activeMedia = e.usage;
      keyboard.press(e.usage);
    }
    else {

      if (activeMedia != e.usage) {

        ++hidIgnored;
        return;
      }

      keyboard.release(e.usage);
      activeMedia = 0;
    }
  }
  else {

    uint8_t u = (uint8_t)e.usage;

    if (held[u] == e.down) {

      ++hidIgnored;
      return;
    }

    held[u] = e.down;

    if (e.down) {
      keyboard.press(u);
    }
    else {
      keyboard.release(u);
    }
  }

  ++hidSent;
  lastSend = millis();
}

// ========================================================
// 15. ESTADISTICAS
// ========================================================

void printStats() {

  uint32_t drops;

  noInterrupts();
  drops = edgeDrops;
  interrupts();

  Serial.println();
  Serial.println("======= HP KB9970 V4.4 =======");

  Serial.printf(
    "[PS2] OK=%lu MAKE=%lu BREAK=%lu "
    "START=%lu PARITY=%lu STOP=%lu "
    "CUT=%lu DROP=%lu\n",
    (unsigned long)validBytes,
    (unsigned long)makes,
    (unsigned long)breaks,
    (unsigned long)badStart,
    (unsigned long)badParity,
    (unsigned long)badStop,
    (unsigned long)cutFrames,
    (unsigned long)drops
  );

  Serial.printf(
    "[CLOCK] FALL=%lu RISE=%lu SHORT=%lu\n",
    (unsigned long)falls,
    (unsigned long)rises,
    (unsigned long)shortClocks
  );

  Serial.printf(
    "[BLE] CONNECTED=%d PAIRED=%d "
    "SENT=%lu DROPS=%lu\n",
    keyboard.isConnected(),
    keyboard.isPaired(),
    (unsigned long)hidSent,
    (unsigned long)hidDrops
  );

  Serial.printf(
    "[HOST] CAPS=%d NUM=%d SCROLL=%d "
    "MASK=0x%02X\n",
    keyboard.isCapsLockOn(),
    keyboard.isNumLockOn(),
    keyboard.isScrollLockOn(),
    readHostLEDs()
  );

  Serial.printf(
    "[TX] STATE=%u ECHO=%d LED=%d "
    "AUTO=%d BLOCKED=%d "
    "CONFIRMED=0x%02X "
    "OK=%lu ERRORS=%lu\n",
    (unsigned)txState,
    echoPassed,
    ledPassed,
    autoLED,
    txBlocked,
    confirmedLED,
    (unsigned long)commandOK,
    (unsigned long)commandErrors
  );

  Serial.printf(
    "[STARTUP] PENDING=%d DELAY=%lu ms\n",
    startupPending,
    (unsigned long)AUTO_START_DELAY_MS
  );

  Serial.println("==============================");
}

// ========================================================
// 16. SETUP
// ========================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(CLOCK_PIN, INPUT_PULLUP);

  keyboard.setLogLevel(
    HIDLogLevel::Normal
  );

  keyboard.setSecurityMode(
    HIDSecurity::JustWorks
  );

  keyboard.setConnIntervalMs(30);
  keyboard.onPairingComplete(paired);

  keyboard.begin();

  attachInterrupt(
    digitalPinToInterrupt(CLOCK_PIN),
    onClockEdge,
    CHANGE
  );

  Serial.println();
  Serial.println("================================");
  Serial.println(" HP KB9970 BLUETOOTH V4.4");
  Serial.println(" LED AUTOMATICOS");
  Serial.println("================================");

  Serial.println(
    "[BLE] Nombre: HP KB9970 V3"
  );

  Serial.println(
    "[AUTO] Sincronizacion habilitada."
  );

  Serial.println(
    "[AUTO] Esperando conexion Bluetooth..."
  );

  Serial.println();
  Serial.println("Comandos disponibles:");
  Serial.println("s = estadisticas");
  Serial.println("d = debug de teclas");
  Serial.println("a = activar/desactivar LED automaticos");
  Serial.println("e = repetir ECHO manualmente");
  Serial.println("l = sincronizar LED manualmente");
}

// ========================================================
// 17. LOOP
// ========================================================

void loop() {

  // Primero escuchar el teclado.
  drainPS2();

  // Procesar sus eventos Bluetooth.
  serviceHID();

  // Detectar nuevas conexiones/reconexiones.
  serviceAutoStartup();

  // Enviar comandos PS/2 cuando corresponda.
  servicePS2Commands();

  // Resultado del pairing.
  if (pairingResult != -1) {

    int8_t result = pairingResult;
    pairingResult = -1;

    Serial.println(
      result
        ? "[BLE] Pairing correcto."
        : "[BLE] Pairing fallido."
    );
  }

  // -----------------------------------------------
  // COMANDOS MANUALES
  // -----------------------------------------------

  while (Serial.available()) {

    char c = Serial.read();

    if (c == 's' || c == 'S') {
      printStats();
    }

    if (c == 'd' || c == 'D') {

      debugKeys = !debugKeys;

      Serial.printf(
        "[DEBUG] %s\n",
        debugKeys ? "ON" : "OFF"
      );
    }

    if (c == 'a' || c == 'A') {

      if (txBlocked) {

        Serial.println(
          "[AUTO] Bloqueado por un error PS/2."
        );
      }
      else {

        autoLED = !autoLED;

        Serial.printf(
          "[AUTO] LED automaticos: %s\n",
          autoLED ? "ON" : "OFF"
        );

        // Si se reactiva manualmente despues
        // del ECHO, solicitar sincronizacion.
        if (autoLED && echoPassed) {

          requestedLED = readHostLEDs();
          ledRequested = true;
        }
      }
    }

    if (c == 'e' || c == 'E') {

      if (txBlocked) {

        Serial.println(
          "[TX] Bloqueado por error."
        );
      }
      else if (txState != TX_IDLE) {

        Serial.println(
          "[TX] Otra orden esta en curso."
        );
      }
      else {

        echoRequested = true;

        Serial.println(
          "[TX] ECHO manual solicitado."
        );
      }
    }

    if (c == 'l' || c == 'L') {

      if (
        txBlocked ||
        !echoPassed ||
        txState != TX_IDLE
      ) {

        Serial.println(
          "[LED] ECHO necesario o TX ocupado."
        );
      }
      else {

        requestedLED = readHostLEDs();
        ledRequested = true;

        Serial.printf(
          "[LED] Sincronizacion manual: 0x%02X\n",
          requestedLED
        );
      }
    }
  }

  // Estadisticas cada 10 segundos.
  static uint32_t lastStats = 0;

  if ((uint32_t)(
    millis() - lastStats
  ) >= 10000) {

    lastStats = millis();
    printStats();
  }

  delay(1);
}
