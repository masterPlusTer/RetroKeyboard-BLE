
#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>

// =====================================================
// HP KB-9970 -> ESP32-C3 SuperMini -> Bluetooth HID
//
// Bibliotecas:
// NimBLE-Arduino 2.5.2
// HijelHID_BLEKeyboard 0.6.0
//
// DATA  = GPIO4
// CLOCK = GPIO3
// GND comun
//
// Alimentacion experimental del HP: 3.3 V
// Su alimentacion nominal es 5 V.
// =====================================================

constexpr uint8_t DATA_PIN = 4;
constexpr uint8_t CLOCK_PIN = 3;

// =====================================================
// 1. CAPTURA PS/2 POR AMBOS FLANCOS
// =====================================================

constexpr uint16_t N_SAMPLES = 1024;
constexpr uint16_t MASK_SAMPLES = N_SAMPLES - 1;

struct Sample {
  uint32_t us;
  uint8_t clk;
  uint8_t data;
};

volatile Sample fifo[N_SAMPLES];

volatile uint16_t wr = 0;
volatile uint16_t rd = 0;
volatile uint32_t overflowPS2 = 0;

void ARDUINO_ISR_ATTR onClock() {
  const uint16_t next =
    (wr + 1) & MASK_SAMPLES;

  if (next == rd) {
    ++overflowPS2;
    return;
  }

  const uint8_t clk = digitalRead(CLOCK_PIN);
  const uint8_t data = digitalRead(DATA_PIN);
  const uint32_t us = micros();

  fifo[wr].us = us;
  fifo[wr].clk = clk;
  fifo[wr].data = data;

  wr = next;
}

bool popSample(Sample &s) {
  noInterrupts();

  if (rd == wr) {
    interrupts();
    return false;
  }

  const uint16_t i = rd;

  s.us = fifo[i].us;
  s.clk = fifo[i].clk;
  s.data = fifo[i].data;

  rd = (i + 1) & MASK_SAMPLES;

  interrupts();
  return true;
}

// =====================================================
// 2. BIBLIOTECA BLUETOOTH HID
// =====================================================

HijelHID_BLEKeyboard keyboard(
  "HP KB9970 V3",
  "KB9970 DIY",
  100
);

// El 100 es un valor fijo de prueba.
// No mide ninguna bateria real.

volatile int8_t pairingResult = -1;

void paired(bool ok) {
  pairingResult = ok ? 1 : 0;
}

// =====================================================
// 3. COLA DE EVENTOS HID
// =====================================================

struct KeyEvent {
  uint8_t usage;
  bool pressed;
};

constexpr uint16_t N_EVENTS = 256;
constexpr uint16_t MASK_EVENTS = N_EVENTS - 1;

KeyEvent eventFifo[N_EVENTS];

uint16_t eventWrite = 0;
uint16_t eventRead = 0;

uint32_t overflowsHID = 0;
uint32_t reportsHID = 0;
uint32_t ignoredHID = 0;

bool held[256] = {false};
bool needResync = false;

void queueKey(uint8_t usage, bool pressed) {
  // No enviar teclas antes de que BLE haya
  // completado el emparejamiento y cifrado.
  if (!keyboard.isPaired()) {
    return;
  }

  const uint16_t next =
    (eventWrite + 1) & MASK_EVENTS;

  if (next == eventRead) {
    ++overflowsHID;

    eventRead = eventWrite;
    needResync = true;

    return;
  }

  eventFifo[eventWrite] = {
    usage,
    pressed
  };

  eventWrite = next;
}

// =====================================================
// 4. PS/2 SCAN CODE SET 2 -> USB HID
// =====================================================

uint8_t toHID(uint8_t c, bool ext) {
  if (ext) {
    switch (c) {
      case 0x11: return 0xE6; // Alt derecho
      case 0x14: return 0xE4; // Ctrl derecho
      case 0x1F: return 0xE3; // Windows izquierdo
      case 0x27: return 0xE7; // Windows derecho
      case 0x2F: return 0x65; // Menu

      case 0x4A: return 0x54; // / numerico
      case 0x5A: return 0x58; // Enter numerico

      case 0x69: return 0x4D; // Fin
      case 0x6B: return 0x50; // Izquierda
      case 0x6C: return 0x4A; // Inicio
      case 0x70: return 0x49; // Insertar
      case 0x71: return 0x4C; // Suprimir
      case 0x72: return 0x51; // Abajo
      case 0x74: return 0x4F; // Derecha
      case 0x75: return 0x52; // Arriba
      case 0x7A: return 0x4E; // AvPag
      case 0x7D: return 0x4B; // RePag

      case 0x7C: return 0x46; // Impr Pant

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

    // Control y puntuacion
    case 0x5A: return 0x28; // Enter
    case 0x76: return 0x29; // Escape
    case 0x66: return 0x2A; // Retroceso
    case 0x0D: return 0x2B; // Tab
    case 0x29: return 0x2C; // Espacio
    case 0x4E: return 0x2D; // -
    case 0x55: return 0x2E; // =
    case 0x54: return 0x2F; // [
    case 0x5B: return 0x30; // ]
    case 0x5D: return 0x31; // Barra invertida
    case 0x61: return 0x64; // Tecla ISO adicional
    case 0x4C: return 0x33; // ;
    case 0x52: return 0x34; // Apostrofo
    case 0x0E: return 0x35; // Acento grave
    case 0x41: return 0x36; // Coma
    case 0x49: return 0x37; // Punto
    case 0x4A: return 0x38; // /
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
    case 0x69: return 0x59; // 1
    case 0x72: return 0x5A; // 2
    case 0x7A: return 0x5B; // 3
    case 0x6B: return 0x5C; // 4
    case 0x73: return 0x5D; // 5
    case 0x74: return 0x5E; // 6
    case 0x6C: return 0x5F; // 7
    case 0x75: return 0x60; // 8
    case 0x7D: return 0x61; // 9
    case 0x70: return 0x62; // 0
    case 0x71: return 0x63; // .

    // Modificadores
    case 0x14: return 0xE0; // Ctrl izquierdo
    case 0x12: return 0xE1; // Shift izquierdo
    case 0x11: return 0xE2; // Alt izquierdo
    case 0x59: return 0xE5; // Shift derecho

    case 0x7E: return 0x47; // Scroll Lock

    default: return 0;
  }
}

// =====================================================
// 5. RECONSTRUCCION DE BYTES PS/2
// =====================================================

uint32_t okBytes = 0;
uint32_t badStart = 0;
uint32_t badParity = 0;
uint32_t badStop = 0;

uint32_t cutFrames = 0;
uint32_t falls = 0;
uint32_t rises = 0;
uint32_t shortFalling = 0;

uint32_t makes = 0;
uint32_t breaks = 0;
uint32_t unmapped = 0;
uint32_t pauses = 0;

uint32_t lastFall = 0;
bool lastFallValid = false;

bool prefixE0 = false;
bool prefixF0 = false;

uint8_t bits = 0;
uint8_t frameValue = 0;
uint8_t parity = 0;

uint8_t pauseLeft = 0;

void resetFrame() {
  bits = 0;
  frameValue = 0;
  parity = 0;
}

void onByte(uint8_t b) {
  // PAUSE tiene una secuencia especial de ocho bytes.
  // Por ahora se reconoce pero no se envia a macOS.
  if (pauseLeft) {
    --pauseLeft;

    if (!pauseLeft) {
      ++pauses;
    }

    return;
  }

  if (b == 0xE1) {
    pauseLeft = 7;
    prefixE0 = false;
    prefixF0 = false;
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

  const bool down = !prefixF0;
  const bool ext = prefixE0;

  prefixE0 = false;
  prefixF0 = false;

  // Print Screen incluye un Shift ficticio.
  // No enviarlo como una tecla real.
  if (ext && b == 0x12) {
    return;
  }

  if (down) {
    ++makes;
  } else {
    ++breaks;
  }

  uint8_t usage = toHID(b, ext);

  if (usage == 0) {
    ++unmapped;
    return;
  }

  queueKey(usage, down);
}

void onFalling(const Sample &s) {
  ++falls;

  if (lastFallValid) {
    uint32_t dt = s.us - lastFall;

    if (dt < 65) {
      ++shortFalling;
    }

    if (dt > 300 && bits != 0) {
      ++cutFrames;

      resetFrame();
      prefixE0 = false;
      prefixF0 = false;
    }
  }

  lastFall = s.us;
  lastFallValid = true;

  const uint8_t bit = s.data & 1;

  if (bits == 0) {
    if (bit != 0) {
      ++badStart;
      return;
    }

    frameValue = 0;
    parity = 0;
  }
  else if (bits <= 8) {
    frameValue |=
      (uint8_t)(bit << (bits - 1));

    parity ^= bit;
  }
  else if (bits == 9) {
    parity ^= bit;
  }

  ++bits;

  if (bits == 11) {
    if (bit != 1) {
      ++badStop;
      prefixE0 = false;
      prefixF0 = false;
    }
    else if (parity != 1) {
      ++badParity;
      prefixE0 = false;
      prefixF0 = false;
    }
    else {
      ++okBytes;
      onByte(frameValue);
    }

    resetFrame();
  }
}

// Vaciar la cola sin bloquear el Bluetooth.
void drainPS2() {
  Sample s;
  uint16_t processed = 0;

  while (processed < 512 && popSample(s)) {
    if (s.clk == 0) {
      onFalling(s);
    } else {
      ++rises;
    }

    ++processed;
  }
}

// =====================================================
// 6. ENVIO HID ORDENADO Y RECUPERACION
// =====================================================

void serviceHID() {
  static bool wasPaired = false;
  static uint32_t lastReport = 0;

  const bool ready = keyboard.isPaired();

  // Detectar enlace listo o enlace perdido.
  if (ready != wasPaired) {
    eventRead = eventWrite;
    memset(held, 0, sizeof(held));

    needResync = false;
    wasPaired = ready;

    Serial.println(
      ready
        ? "[BLE] EMPAREJADO Y LISTO"
        : "[BLE] ENLACE NO LISTO"
    );

    if (ready) {
      keyboard.releaseAll();
      lastReport = millis();
    }
  }

  if (!ready) {
    return;
  }

  // Si la cola se lleno, liberar todas las
  // teclas para evitar teclas pegadas.
  if (needResync) {
    keyboard.releaseAll();

    eventRead = eventWrite;
    memset(held, 0, sizeof(held));

    needResync = false;
    lastReport = millis();

    return;
  }

  // Mantener separados MAKE y BREAK.
  // Una notificacion aproximadamente cada 20 ms.
  if ((uint32_t)(millis() - lastReport) < 20) {
    return;
  }

  if (eventRead == eventWrite) {
    return;
  }

  KeyEvent ev = eventFifo[eventRead];

  eventRead =
    (eventRead + 1) & MASK_EVENTS;

  // No enviar dos veces un MAKE si la tecla
  // ya esta pulsada, ni un BREAK redundante.
  if (held[ev.usage] == ev.pressed) {
    ++ignoredHID;
    return;
  }

  held[ev.usage] = ev.pressed;

  if (ev.pressed) {
    keyboard.press(ev.usage);
  } else {
    keyboard.release(ev.usage);
  }

  ++reportsHID;
  lastReport = millis();
}

// =====================================================
// 7. ESTADISTICAS
// =====================================================

void printStats() {
  uint32_t drops;

  noInterrupts();
  drops = overflowPS2;
  interrupts();

  Serial.printf(
    "[PS2] OK=%lu MAKE=%lu BREAK=%lu "
    "INICIO=%lu PARIDAD=%lu PARADA=%lu "
    "CORTADAS=%lu COLA=%lu\n",
    (unsigned long)okBytes,
    (unsigned long)makes,
    (unsigned long)breaks,
    (unsigned long)badStart,
    (unsigned long)badParity,
    (unsigned long)badStop,
    (unsigned long)cutFrames,
    (unsigned long)drops
  );

  Serial.printf(
    "[CLK] BAJADAS=%lu SUBIDAS=%lu CORTOS=%lu\n",
    (unsigned long)falls,
    (unsigned long)rises,
    (unsigned long)shortFalling
  );

  Serial.printf(
    "[BLE] CONECTADO=%d EMPAREJADO=%d "
    "REPORTES=%lu COLA_PERDIDA=%lu "
    "DUPLICADOS=%lu SIN_MAPA=%lu PAUSE=%lu\n",
    keyboard.isConnected(),
    keyboard.isPaired(),
    (unsigned long)reportsHID,
    (unsigned long)overflowsHID,
    (unsigned long)ignoredHID,
    (unsigned long)unmapped,
    (unsigned long)pauses
  );
}

// =====================================================
// 8. SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(CLOCK_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println(
    "=== HP KB9970 V3: PS/2 + BLE HID ==="
  );

  Serial.println(
    "NimBLE 2.5.2 + HijelHID 0.6.0"
  );

  // Biblioteca BLE probada con macOS.
  keyboard.setLogLevel(HIDLogLevel::Normal);

  // Emparejamiento Just Works.
  keyboard.setSecurityMode(
    HIDSecurity::JustWorks
  );

  // Intervalo moderado para estabilidad.
  // El anfitrion decide si acepta el valor.
  keyboard.setConnIntervalMs(30);

  // Informar si la seguridad se completa.
  keyboard.onPairingComplete(paired);

  // Inicializar BLE antes de activar PS/2.
  keyboard.begin();

  attachInterrupt(
    digitalPinToInterrupt(CLOCK_PIN),
    onClock,
    CHANGE
  );

  Serial.println(
    "Buscar: HP KB9970 V3 en Bluetooth"
  );

  Serial.println(
    "Comandos: s=estadisticas, "
    "x=borrar vinculos del ESP32"
  );
}

// =====================================================
// 9. LOOP
// =====================================================

void loop() {
  // Primera prioridad: recibir sin perder bits.
  drainPS2();

  // Segunda prioridad: enviar teclas por BLE.
  serviceHID();

  // Resultado del emparejamiento.
  if (pairingResult != -1) {
    int8_t result = pairingResult;
    pairingResult = -1;

    Serial.println(
      result
        ? "[BLE] EMPAREJAMIENTO CORRECTO"
        : "[BLE] EMPAREJAMIENTO FALLIDO"
    );
  }

  // Comandos por USB.
  while (Serial.available()) {
    char c = Serial.read();

    if (c == 's' || c == 'S') {
      printStats();
    }

    if (c == 'x' || c == 'X') {
      if (keyboard.isConnected()) {
        Serial.println(
          "[BLE] Desconecta desde macOS "
          "antes de borrar vinculos."
        );
      } else {
        keyboard.clearBonds();

        Serial.println(
          "[BLE] Vinculos locales borrados."
        );

        Serial.println(
          "[BLE] Si macOS lo tenia guardado, "
          "selecciona Olvidar dispositivo."
        );
      }
    }
  }

  // Diagnostico periodico.
  static uint32_t last = 0;

  if (millis() - last >= 10000) {
    last = millis();
    printStats();
  }

  // El ESP32-C3 tiene un solo nucleo.
  // Dar tiempo al proceso Bluetooth.
  delay(1);
}
