
#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>

// ===================================================
// HP KB-9970 Bluetooth V4.1
// ESP32-C3 SuperMini
//
// PS/2 DATA  = GPIO4
// PS/2 CLOCK = GPIO3
//
// NimBLE-Arduino 2.x
// HijelHID_BLEKeyboard 0.6.x
//
// Alimentacion actual del HP: 3.3 V experimental
// Alimentacion nominal del HP: 5 V
// ===================================================

constexpr uint8_t DATA_PIN = 4;
constexpr uint8_t CLOCK_PIN = 3;

// ---------------------------------------------------
// IMPORTANTE:
// Declarar las estructuras ANTES de los prototipos.
// Esto evita el error de Arduino IDE con "Sample".
// ---------------------------------------------------

struct PS2Edge {
  uint32_t timeUs;
  uint8_t clockLevel;
  uint8_t dataLevel;
};

struct HIDEvent {
  uint16_t usage;
  bool pressed;
  bool media;
};

// Prototipos explicitos
bool popPS2Edge(PS2Edge &edge);
void processFallingEdge(const PS2Edge &edge);
void processPS2Byte(uint8_t code);
void processKey(uint8_t code, bool ext, bool down);
uint8_t mapKeyboard(uint8_t code, bool ext);
uint16_t mapMedia(uint8_t code, bool ext);
void queueHID(uint16_t usage, bool down, bool media);
void drainPS2();
void serviceHID();
void printStats();
void resetParser();
void onPaired(bool success);

// ===================================================
// 1. BLUETOOTH
// ===================================================

// Conservar nombre de V3.
// No cambiar bibliotecas ni conexiones.
HijelHID_BLEKeyboard keyboard(
  "HP KB9970 V3",
  "KB9970 DIY",
  100
);

// El 100 es un valor de prueba, no una
// lectura real de bateria.

volatile int8_t pairingResult = -1;

void onPaired(bool success) {
  pairingResult = success ? 1 : 0;
}

// ===================================================
// 2. CAPTURA PS/2 POR AMBOS FLANCOS
// ===================================================

constexpr uint16_t SAMPLE_SIZE = 1024;
constexpr uint16_t SAMPLE_MASK = SAMPLE_SIZE - 1;

volatile PS2Edge sampleQueue[SAMPLE_SIZE];

volatile uint16_t sampleWrite = 0;
volatile uint16_t sampleRead = 0;
volatile uint32_t sampleDrops = 0;

void ARDUINO_ISR_ATTR onClockChange() {
  uint16_t next =
    (sampleWrite + 1) & SAMPLE_MASK;

  if (next == sampleRead) {
    ++sampleDrops;
    return;
  }

  uint8_t clk = digitalRead(CLOCK_PIN);
  uint8_t data = digitalRead(DATA_PIN);
  uint32_t t = micros();

  sampleQueue[sampleWrite].timeUs = t;
  sampleQueue[sampleWrite].clockLevel = clk;
  sampleQueue[sampleWrite].dataLevel = data;

  sampleWrite = next;
}

bool popPS2Edge(PS2Edge &edge) {
  noInterrupts();

  if (sampleRead == sampleWrite) {
    interrupts();
    return false;
  }

  uint16_t i = sampleRead;

  edge.timeUs = sampleQueue[i].timeUs;
  edge.clockLevel = sampleQueue[i].clockLevel;
  edge.dataLevel = sampleQueue[i].dataLevel;

  sampleRead = (i + 1) & SAMPLE_MASK;

  interrupts();
  return true;
}

// ===================================================
// 3. COLA DE EVENTOS BLUETOOTH
// ===================================================

constexpr uint16_t EVENT_SIZE = 256;
constexpr uint16_t EVENT_MASK = EVENT_SIZE - 1;

HIDEvent eventQueue[EVENT_SIZE];

uint16_t eventWrite = 0;
uint16_t eventRead = 0;

bool heldKeys[256] = {false};
uint16_t activeMedia = 0;

bool needResync = false;

uint32_t eventDrops = 0;
uint32_t reportsSent = 0;
uint32_t ignoredEvents = 0;
uint32_t unmappedKeys = 0;
uint32_t mediaEvents = 0;

void clearLocalHID() {
  memset(heldKeys, 0, sizeof(heldKeys));

  activeMedia = 0;
  eventRead = eventWrite;
  needResync = false;
}

void queueHID(
  uint16_t usage,
  bool down,
  bool media
) {
  if (usage == 0) return;
  if (!keyboard.isPaired()) return;

  uint16_t next =
    (eventWrite + 1) & EVENT_MASK;

  if (next == eventRead) {
    ++eventDrops;
    eventRead = eventWrite;
    needResync = true;
    return;
  }

  eventQueue[eventWrite].usage = usage;
  eventQueue[eventWrite].pressed = down;
  eventQueue[eventWrite].media = media;

  eventWrite = next;
}

// ===================================================
// 4. PS/2 SCAN CODE SET 2 -> USB HID
// ===================================================

uint8_t mapKeyboard(uint8_t c, bool ext) {
  if (ext) {
    switch (c) {
      // Modificadores
      case 0x11: return 0xE6; // Right Alt
      case 0x14: return 0xE4; // Right Ctrl
      case 0x1F: return 0xE3; // Left GUI
      case 0x27: return 0xE7; // Right GUI
      case 0x2F: return 0x65; // Menu

      // Numpad
      case 0x4A: return 0x54; // /
      case 0x5A: return 0x58; // Enter

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

      case 0x7C: return 0x46; // Print Screen

      default: return 0;
    }
  }

  switch (c) {
    // Letras
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

    // Numeros superiores
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

    // Edicion y puntuacion
    case 0x5A: return 0x28; // Enter
    case 0x76: return 0x29; // Escape
    case 0x66: return 0x2A; // Backspace
    case 0x0D: return 0x2B; // Tab
    case 0x29: return 0x2C; // Space

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

    case 0x61: return 0x64; // Tecla ISO extra
    case 0x58: return 0x39; // Caps Lock

    // F1 a F12
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

    // Teclado numerico
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

    // Modificadores
    case 0x14: return 0xE0; // Left Ctrl
    case 0x12: return 0xE1; // Left Shift
    case 0x11: return 0xE2; // Left Alt
    case 0x59: return 0xE5; // Right Shift

    case 0x7E: return 0x47; // Scroll Lock

    default: return 0;
  }
}

// ===================================================
// 5. MULTIMEDIA PS/2 -> HID CONSUMER
// ===================================================

uint16_t mapMedia(uint8_t c, bool ext) {
  if (!ext) return 0;

  switch (c) {
    // Multimedia
    case 0x4D: return MEDIA_NEXT_TRACK;
    case 0x15: return MEDIA_PREV_TRACK;
    case 0x3B: return MEDIA_STOP;
    case 0x34: return MEDIA_PLAY_PAUSE;
    case 0x50: return MEDIA_MEDIA_SELECT;

    // Audio
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

    // No activar funciones de apagado.
    // Si el HP envia E0 3F, dejarlo sin mapear
    // hasta confirmar que tecla fisica es.
    default: return 0;
  }
}

// ===================================================
// 6. INTERPRETACION PS/2
// ===================================================

bool prefixE0 = false;
bool prefixF0 = false;

uint32_t lastByteTime = 0;

uint32_t keyMakes = 0;
uint32_t keyBreaks = 0;
uint32_t pauseCount = 0;

bool debugKeys = false;

// Pause:
// E1 14 77 E1 F0 14 F0 77
const uint8_t PAUSE_SEQ[8] = {
  0xE1, 0x14, 0x77, 0xE1,
  0xF0, 0x14, 0xF0, 0x77
};

uint8_t pausePosition = 0;

void resetParser() {
  prefixE0 = false;
  prefixF0 = false;
  pausePosition = 0;
}

void processKey(
  uint8_t code,
  bool ext,
  bool down
) {
  // E0 12 es un Shift ficticio usado
  // en Print Screen. No enviarlo.
  if (ext && code == 0x12) return;

  if (down) {
    ++keyMakes;
  } else {
    ++keyBreaks;
  }

  uint16_t media = mapMedia(code, ext);

  if (media != 0) {
    ++mediaEvents;

    queueHID(media, down, true);

    if (debugKeys) {
      Serial.printf(
        "[MEDIA] %s E0 %02X -> %04X\n",
        down ? "MAKE" : "BREAK",
        code,
        media
      );
    }

    return;
  }

  uint8_t usage = mapKeyboard(code, ext);

  if (usage == 0) {
    ++unmappedKeys;

    Serial.printf(
      "[PS2] SIN MAPEAR: %s%02X (%s)\n",
      ext ? "E0 " : "",
      code,
      down ? "MAKE" : "BREAK"
    );

    return;
  }

  queueHID(usage, down, false);

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

void processPS2Byte(uint8_t code) {
  uint32_t now = micros();

  // Descartar prefijos caducados.
  if (lastByteTime != 0 &&
      (uint32_t)(now - lastByteTime) > 250000) {
    resetParser();
  }

  lastByteTime = now;

  // Manejar Pause, que no tiene BREAK normal.
  if (pausePosition != 0 || code == 0xE1) {
    if (code == PAUSE_SEQ[pausePosition]) {
      ++pausePosition;

      if (pausePosition == 8) {
        pausePosition = 0;
        ++pauseCount;

        // USB HID Pause = 0x48.
        queueHID(0x48, true, false);
        queueHID(0x48, false, false);

        Serial.println("[PS2] PAUSE");
      }

      return;
    }

    // Secuencia anomala: recuperar estado.
    resetParser();
    return;
  }

  if (code == 0xE0) {
    prefixE0 = true;
    return;
  }

  if (code == 0xF0) {
    prefixF0 = true;
    return;
  }

  bool ext = prefixE0;
  bool down = !prefixF0;

  prefixE0 = false;
  prefixF0 = false;

  processKey(code, ext, down);
}

// ===================================================
// 7. RECEPTOR DE TRAMAS DE 11 BITS
// ===================================================

uint32_t validBytes = 0;
uint32_t badStart = 0;
uint32_t badParity = 0;
uint32_t badStop = 0;
uint32_t cutFrames = 0;

uint32_t clockFalls = 0;
uint32_t clockRises = 0;
uint32_t shortClocks = 0;

uint32_t lastFall = 0;
bool haveLastFall = false;

uint8_t bitIndex = 0;
uint8_t currentByte = 0;
uint8_t parityValue = 0;

void resetFrame() {
  bitIndex = 0;
  currentByte = 0;
  parityValue = 0;
}

void processFallingEdge(const PS2Edge &edge) {
  ++clockFalls;

  if (haveLastFall) {
    uint32_t dt = edge.timeUs - lastFall;

    if (dt < 65) {
      ++shortClocks;
    }

    if (dt > 300 && bitIndex != 0) {
      ++cutFrames;
      resetFrame();
      resetParser();
    }
  }

  lastFall = edge.timeUs;
  haveLastFall = true;

  uint8_t bit = edge.dataLevel & 1;

  if (bitIndex == 0) {
    if (bit != 0) {
      ++badStart;
      return;
    }

    currentByte = 0;
    parityValue = 0;
  }
  else if (bitIndex <= 8) {
    currentByte |=
      (uint8_t)(bit << (bitIndex - 1));

    parityValue ^= bit;
  }
  else if (bitIndex == 9) {
    parityValue ^= bit;
  }

  ++bitIndex;

  if (bitIndex == 11) {
    if (bit != 1) {
      ++badStop;
      resetParser();
    }
    else if (parityValue != 1) {
      ++badParity;
      resetParser();
    }
    else {
      ++validBytes;
      processPS2Byte(currentByte);
    }

    resetFrame();
  }
}

void drainPS2() {
  PS2Edge edge;
  uint16_t count = 0;

  // Procesar como maximo 512 muestras
  // por iteracion para dar tiempo a BLE.
  while (count < 512 && popPS2Edge(edge)) {
    if (edge.clockLevel == 0) {
      processFallingEdge(edge);
    } else {
      ++clockRises;
    }

    ++count;
  }
}

// ===================================================
// 8. ENVIAR TECLAS POR BLUETOOTH
// ===================================================

void serviceHID() {
  static bool wasPaired = false;
  static uint32_t lastSend = 0;

  bool ready = keyboard.isPaired();

  if (ready != wasPaired) {
    wasPaired = ready;

    clearLocalHID();

    if (ready) {
      keyboard.releaseAll();

      Serial.println(
        "[BLE] EMPAREJADO Y LISTO"
      );
    } else {
      Serial.println(
        "[BLE] ENLACE NO LISTO"
      );
    }

    lastSend = millis();
  }

  if (!ready) return;

  // Recuperar de un desbordamiento.
  if (needResync) {
    keyboard.releaseAll();
    clearLocalHID();
    lastSend = millis();
    return;
  }

  // Conservar el ritmo que ya funciono en V3.
  if ((uint32_t)(millis() - lastSend) < 20) {
    return;
  }

  if (eventRead == eventWrite) return;

  HIDEvent ev = eventQueue[eventRead];

  eventRead = (eventRead + 1) & EVENT_MASK;

  if (ev.media) {
    if (ev.pressed) {
      if (activeMedia == ev.usage) {
        ++ignoredEvents;
        return;
      }

      if (activeMedia != 0) {
        keyboard.release((uint16_t)activeMedia);
      }

      activeMedia = ev.usage;
      keyboard.press((uint16_t)ev.usage);

    } else {
      if (activeMedia != ev.usage) {
        ++ignoredEvents;
        return;
      }

      keyboard.release((uint16_t)ev.usage);
      activeMedia = 0;
    }
  } else {
    uint8_t usage = (uint8_t)ev.usage;

    if (heldKeys[usage] == ev.pressed) {
      ++ignoredEvents;
      return;
    }

    heldKeys[usage] = ev.pressed;

    if (ev.pressed) {
      keyboard.press((uint8_t)usage);
    } else {
      keyboard.release((uint8_t)usage);
    }
  }

  ++reportsSent;
  lastSend = millis();
}

// ===================================================
// 9. ESTADISTICAS
// ===================================================

void printStats() {
  uint32_t drops;

  noInterrupts();
  drops = sampleDrops;
  interrupts();

  Serial.println();
  Serial.println("===== HP KB9970 V4.1 =====");

  Serial.printf(
    "[PS2] BYTES=%lu MAKE=%lu BREAK=%lu "
    "PAUSE=%lu\n",
    (unsigned long)validBytes,
    (unsigned long)keyMakes,
    (unsigned long)keyBreaks,
    (unsigned long)pauseCount
  );

  Serial.printf(
    "[ERROR] INICIO=%lu PARIDAD=%lu "
    "PARADA=%lu CORTADAS=%lu COLA=%lu\n",
    (unsigned long)badStart,
    (unsigned long)badParity,
    (unsigned long)badStop,
    (unsigned long)cutFrames,
    (unsigned long)drops
  );

  Serial.printf(
    "[CLOCK] BAJADAS=%lu SUBIDAS=%lu "
    "CORTOS=%lu\n",
    (unsigned long)clockFalls,
    (unsigned long)clockRises,
    (unsigned long)shortClocks
  );

  Serial.printf(
    "[HID] CONECTADO=%d PAIRED=%d "
    "REPORTES=%lu MEDIA=%lu "
    "SIN_MAPA=%lu PERDIDOS=%lu "
    "IGNORADOS=%lu\n",
    keyboard.isConnected(),
    keyboard.isPaired(),
    (unsigned long)reportsSent,
    (unsigned long)mediaEvents,
    (unsigned long)unmappedKeys,
    (unsigned long)eventDrops,
    (unsigned long)ignoredEvents
  );

  Serial.printf(
    "[LEDS HOST] CAPS=%d NUM=%d SCROLL=%d\n",
    keyboard.isCapsLockOn(),
    keyboard.isNumLockOn(),
    keyboard.isScrollLockOn()
  );

  Serial.println("==========================");
}

// ===================================================
// 10. SETUP
// ===================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(CLOCK_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("==============================");
  Serial.println(" HP KB9970 BLUETOOTH V4.1");
  Serial.println(" ESP32-C3 SUPERMINI");
  Serial.println("==============================");

  keyboard.setLogLevel(HIDLogLevel::Normal);

  keyboard.setSecurityMode(
    HIDSecurity::JustWorks
  );

  keyboard.setConnIntervalMs(30);
  keyboard.onPairingComplete(onPaired);

  keyboard.begin();

  attachInterrupt(
    digitalPinToInterrupt(CLOCK_PIN),
    onClockChange,
    CHANGE
  );

  Serial.println(
    "[BLE] Nombre: HP KB9970 V3"
  );

  Serial.println(
    "Comandos:"
  );

  Serial.println(
    "s = estadisticas"
  );

  Serial.println(
    "d = activar/desactivar debug"
  );

  Serial.println(
    "x = borrar vinculos locales"
  );
}

// ===================================================
// 11. LOOP
// ===================================================

void loop() {
  drainPS2();
  serviceHID();

  if (pairingResult != -1) {
    int8_t result = pairingResult;
    pairingResult = -1;

    Serial.println(
      result
        ? "[BLE] EMPAREJAMIENTO CORRECTO"
        : "[BLE] EMPAREJAMIENTO FALLIDO"
    );
  }

  while (Serial.available()) {
    char c = Serial.read();

    if (c == 's' || c == 'S') {
      printStats();
    }

    if (c == 'd' || c == 'D') {
      debugKeys = !debugKeys;

      Serial.printf(
        "[DEBUG] TECLAS=%s\n",
        debugKeys ? "ON" : "OFF"
      );
    }

    if (c == 'x' || c == 'X') {
      if (keyboard.isConnected()) {
        Serial.println(
          "[BLE] Desconectar antes "
          "de borrar vinculos."
        );
      } else {
        keyboard.clearBonds();

        Serial.println(
          "[BLE] Vinculos locales borrados."
        );
      }
    }
  }

  static uint32_t lastStats = 0;

  if (millis() - lastStats >= 10000) {
    lastStats = millis();
    printStats();
  }

  delay(1);
}
