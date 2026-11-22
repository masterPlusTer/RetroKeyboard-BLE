/*
 * HP KB-9970 V2.0.0 - ESP32-C3 SuperMini / Arduino IDE.
 * Firmware estable V2.0.0 (continuacion de V1 en GitHub). PS/2, HID, LED y energia.
 * PS/2 DATA=GPIO4 CLOCK=GPIO3. CPU160 MHz. HijelHID_BLEKeyboard + NimBLE.
 * Mantener placa, core y particiones que ya usas. Core observado: ESP32 3.3.10.
 *
 * INSTALAR LA VERSION 2.0.0:
 * 1. Guardar en carpeta HP_KB9970_V2_0_0 y abrir este .ino en Arduino.
 * 2. Cargar por USB en el teclado de laboratorio si la web anterior no responde.
 * 3. Al instalar desde cero con NVS limpia, la web se abre una vez automaticamente.
 *    Una actualizacion OTA desde V7.2 conserva la marca de instalacion y arranca en Bluetooth.
 * 4. Conectar Mac/telefono a HP-KB9970-WEB, clave Wi-Fi: teclado1.
 * 5. Abrir http://192.168.4.1/ (HTTP). La pagina abre sin usuario ni clave.
 * 6. Para subir otro firmware: arrastrar su .bin de APLICACION, escribir
 *    teclado en Contraseña de carga y pulsar Actualizar. Mantener alimentacion.
 * 7. Boton Volver al teclado: regresa a Bluetooth. Si no se inicia una carga,
 *    el mantenimiento termina a los 15 minutos. Reinicios posteriores: teclado.
 *
 * VOLVER A MANTENIMIENTO: Ctrl+Alt+F12 durante3s y SOLTAR; o comando serie o.
 * La pagina y /status muestran version, espacio real, tiempo restante y errores.
 * USB no se necesita para las cargas web despues de instalar esta correccion.
 * No cargar merged.bin, bootloader.bin, partitions.bin ni boot_app0.bin.
 * El receptor comprueba cabecera ESP32-C3, longitud, particion y validacion Update.
 * Los nombres de descarga como archivo.ino(1).bin tambien se aceptan.
 *
 * Particiones observadas: dos slots de1.310.720 bytes; NO cambian con esta OTA.
 * El binario compilado debe caber. Este archivo es fuente, no binario precompilado.
 * Wi-Fi TX solicitado34 (8.5dBm), igual al ajuste que funciono en tu SuperMini.
 * Keep-alive CPU deja de generar carga tras20min sin actividad real. No garantiza
 * el apagado/encendido del power bank: depende de su circuito y consumo restante.
 * Claves sencillas pedidas para este montaje: WiFi teclado1; actualizacion teclado.
 * Para conservar el acceso web, las futuras versiones deben incluir este receptor.
 * Validacion de logica mediante pruebas en PC; falta prueba fisica ESP32/RF/flash.
 */
#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp32-hal-cpu.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#if !defined(ARDUINO_ARCH_ESP32)
#error "Elegir una placa ESP32-C3 del paquete esp32 de Espressif."
#endif

// ================= CONFIGURACIÓN =================
constexpr uint8_t PIN_DATA = 4;
constexpr uint8_t PIN_CLOCK = 3;
constexpr uint32_t IDLE_TIMEOUT_MS = 20UL * 60UL * 1000UL;
constexpr uint16_t BLE_INTERVAL_REQUEST_MS = 8; // HijelHID redondea a 7.5 ms; decide el host.
constexpr uint8_t HID_GAP_DEFAULT_MS = 4;       // 8 ms anterior; serie '3' vuelve a 8 ms.
constexpr bool BLE_WARM_REPORTS_DEFAULT = true;
constexpr uint32_t BLE_WARM_INTERVAL_MS = 500; // Informe VACÍO, sólo con todas las teclas arriba.
constexpr uint32_t HID_STALE_MS = 500;
constexpr uint32_t KEEP_INTERVAL_MS = 18000;  // Descanso entre fin e inicio de ventanas.
constexpr uint32_t KEEP_DURATION_MS = 4000;
constexpr uint32_t KEEP_SLICE_US = 1500;
constexpr uint32_t OTA_WINDOW_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t OTA_TRANSFER_LIMIT_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t OTA_HOLD_MS = 3000;
constexpr uint32_t OTA_RELEASE_WAIT_MS = 10000;
constexpr uint32_t OTA_LED_RECOVERY_GRACE_MS = 2000;
constexpr uint32_t OTA_REBOOT_GRACE_MS = 1500;
constexpr char OTA_AP_NAME[] = "HP-KB9970-WEB";
constexpr char OTA_AP_PASSWORD[] = "teclado1";
constexpr char OTA_UPLOAD_PASSWORD[] = "teclado";
constexpr char FW_VERSION[] = "2.0.0";
constexpr uint32_t CPU_TARGET_MHZ = 160;

constexpr uint16_t EDGE_N = 1024, EDGE_MASK = EDGE_N - 1;
constexpr uint16_t HID_N = 256, HID_MASK = HID_N - 1;
constexpr uint32_t PS2_TX_QUIET_US = 20000;
constexpr uint32_t BLE_SETTLE_MS = 2000;
constexpr uint32_t CMD_TIMEOUT_MS = 150;
constexpr uint32_t LED_PARAM_TIMEOUT_MS = 250;
static_assert((EDGE_N & EDGE_MASK) == 0 && (HID_N & HID_MASK) == 0, "Colas potencia de dos");
static_assert(IDLE_TIMEOUT_MS > 0 && IDLE_TIMEOUT_MS < 0x80000000UL, "Timeout invalido");
static_assert(sizeof(OTA_AP_PASSWORD) >= 9 && sizeof(OTA_AP_PASSWORD) <= 64, "WPA2 requiere 8..63 caracteres");
static_assert(sizeof(OTA_UPLOAD_PASSWORD) >= 5, "Clave OTA vacia o demasiado corta");

struct PS2Edge { uint32_t timeUs; uint8_t data; };
struct HIDEvent { uint16_t usage; uint32_t queuedMs; bool down; bool media; };
struct PowerState {
  uint32_t lastActivityMs = 0;
  uint32_t restStartedMs = 0;
  uint32_t windowStartedMs = 0;
  uint32_t pulses = 0;
  bool awake = true;
  bool enabled = true;
  bool active = false;
  bool otaHold = false;
  bool commandPause = false;
  bool pulseRequested = false;
};
enum CmdState : uint8_t { CMD_IDLE, CMD_WAIT_ECHO, CMD_WAIT_ED_ACK, CMD_SEND_MASK, CMD_WAIT_MASK_ACK, CMD_RETRY_BYTE };

// Declaraciones explícitas: también permite probar este .ino como C++ normal.
void logf(const char *fmt, ...);
void logLine(const char *message = "");
void serviceLogOutput();
PowerState powerSnapshot();
bool popEdge(PS2Edge &edge);
void processFall(const PS2Edge &edge);
uint8_t keyUsage(uint8_t c, bool ext);
uint16_t mediaUsage(uint8_t c, bool ext);
void servicePS2();
void serviceHID();
void serviceCommands();
void serviceConnection();
void resetFrame();
void resetParser();
void clearHIDQueue();
void clearPhysicalKeys();
void syncAfterPS2Error();
void processPS2Byte(uint8_t b, uint32_t receivedUs);
void processKey(uint8_t c, bool ext, bool down);
void addHID(uint16_t usage, bool down, bool media = false);
void rebuildHIDFromPhysical();
void setPowerCommandPause(bool paused);
void noteActivity();
void requestOTA();
void serviceOTA();
void serviceOtaShortcut();
void stopOTA();
void feedWatchdog();
bool configureWatchdog(uint32_t timeoutMs);
void printStats();
void printHelp();
void serialCommands();
bool consumeOTABootRequest();
bool persistOTABootRequest();
bool startOTABootMode();
void configureWebOTA();
void serviceWebOTAClient();
void serviceWebUploadTimeout();
void resetWebUploadState();
void finishOtaExit();
void scheduleOtaReboot(const char *why);

HijelHID_BLEKeyboard keyboard("HP KB9970 V3", "KB9970 DIY", 100);
// El 100% sigue siendo simulado: el montaje no aporta medición de batería.
volatile int8_t pairingStatus = -1;
portMUX_TYPE pairingMux = portMUX_INITIALIZER_UNLOCKED;
void pairingDone(bool success) {
  portENTER_CRITICAL(&pairingMux);
  pairingStatus = success ? 1 : 0;
  portEXIT_CRITICAL(&pairingMux);
}

// Logs de mejor esfuerzo: nunca esperar a un monitor serie desconectado.
uint32_t logDrops = 0;
constexpr uint16_t LOG_N = 2048, LOG_MASK = LOG_N - 1;
char logBuffer[LOG_N];
uint16_t logRead = 0, logWrite = 0;
void logf(const char *fmt, ...) {
  if (!Serial) return;
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (n <= 0) return;
  const size_t length = static_cast<size_t>(n) < sizeof(buffer) ? static_cast<size_t>(n) : sizeof(buffer) - 1;
  const uint16_t free = (logRead - logWrite - 1) & LOG_MASK;
  if (length > free) { ++logDrops; return; }
  for (size_t i = 0; i < length; ++i) {
    logBuffer[logWrite] = buffer[i];
    logWrite = (logWrite + 1) & LOG_MASK;
  }
}
void logLine(const char *text) { logf("%s\n", text); }
void serviceLogOutput() {
  if (!Serial) { logRead = logWrite; return; }
  const int available = Serial.availableForWrite();
  if (available <= 0 || logRead == logWrite) return;
  uint16_t count = (logWrite - logRead) & LOG_MASK;
  if (count > 64) count = 64; // trabajo acotado por vuelta
  if (count > static_cast<unsigned>(available)) count = static_cast<uint16_t>(available);
  if (count > LOG_N - logRead) count = LOG_N - logRead;
  const size_t written = Serial.write(reinterpret_cast<const uint8_t *>(logBuffer + logRead), count);
  logRead = (logRead + static_cast<uint16_t>(written)) & LOG_MASK;
}

volatile PS2Edge edges[EDGE_N];
volatile uint16_t edgeW = 0, edgeR = 0;
volatile uint32_t edgeDrops = 0, lastEdgeUs = 0;
volatile bool transmitting = false;
bool ps2ReceiverAttached = false;
bool ps2TxAttempted = false;
void ARDUINO_ISR_ATTR clockISR() {
  if (transmitting) return;
  const uint32_t now = micros();
  lastEdgeUs = now;
  const uint16_t next = (edgeW + 1) & EDGE_MASK;
  if (next == edgeR) { ++edgeDrops; return; }
  edges[edgeW].timeUs = now;
  edges[edgeW].data = static_cast<uint8_t>(digitalRead(PIN_DATA));
  edgeW = next;
}
bool popEdge(PS2Edge &edge) {
  noInterrupts();
  if (edgeR == edgeW) { interrupts(); return false; }
  const uint16_t index = edgeR;
  edge.timeUs = edges[index].timeUs;
  edge.data = edges[index].data;
  edgeR = (index + 1) & EDGE_MASK;
  interrupts();
  return true;
}
void clearEdgeQueue() {
  noInterrupts();
  edgeR = edgeW;
  interrupts();
}

HIDEvent hidEvents[HID_N];
uint16_t hidW = 0, hidR = 0;
bool hidHeld[256] = {}, ps2Held[512] = {};
uint16_t physicalHeldCount = 0, mediaHeld = 0;
bool hidNeedsRelease = false, hidRolloverPending = false;
uint32_t hidDrops = 0, hidSent = 0, ignored = 0, unmapped = 0, resyncs = 0;
uint32_t staleRecoveries = 0, duplicateMakes = 0, warmReports = 0;
uint32_t lastKeyMs = 0, lastSendMs = 0, lastReportAttemptMs = 0;
uint16_t maxQueue = 0;
uint32_t maxQueueWaitMs = 0, queueSamples = 0, maxHidCallUs = 0;
uint64_t sumQueueWaitMs = 0;
uint8_t hidGapMs = HID_GAP_DEFAULT_MS;
bool debugKeys = false, warmReportsEnabled = BLE_WARM_REPORTS_DEFAULT;

uint32_t bytesOK = 0, makes = 0, breaks = 0, pauses = 0;
uint32_t badStart = 0, badParity = 0, badStop = 0, cutFrames = 0;
uint32_t lastFallUs = 0, lastDecodedByteUs = 0, lastDropSeen = 0;
bool haveFall = false, haveDecodedByte = false;
uint8_t bitIndex = 0, frameByte = 0, parityBits = 0;
bool prefixE0 = false, prefixF0 = false;
uint8_t pauseIndex = 0;
const uint8_t PAUSE_SEQ[8] = {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77};

CmdState cmdState = CMD_IDLE;
bool wantEcho = false, wantLED = false, echoOK = false;
bool autoLED = true, txBlocked = false, ledKnown = false, readyPrev = false;
uint8_t ledTarget = 0, ledConfirmed = 0, lastObservedLED = 0;
uint32_t ledObservedAt = 0, commandStartedAt = 0, connectedAt = 0;
uint32_t txOK = 0, txErrors = 0;

PowerState powerState;
portMUX_TYPE powerMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t powerTaskHandle = nullptr;
volatile uint32_t keepCalculation = 0x12345678;
bool powerTaskRunning = false;
bool watchdogEnabled = false;
uint32_t watchdogTimeoutMs = 15000;

bool otaRequested = false, otaMode = false, otaBusy = false, otaListening = false;
bool otaRejected = false, otaErrorPending = false, otaRebootPending = false;
bool otaClosing = false;
uint8_t wifiStopAttempts = 0;
uint32_t otaRequestMs = 0, otaStartMs = 0, otaTransferStartMs = 0;
uint32_t otaRestartMs = 0, wifiStopAttemptMs = 0;
unsigned int otaProgress = 0;
int lastOtaError = -1;
bool otaChordTracking = false, otaChordFired = false, otaChordNeedsRelease = false;
bool otaOwnsF12 = false;
uint32_t otaChordStartMs = 0;

// ============== ACTIVIDAD Y KEEP-ALIVE ==============
void initPowerState(uint32_t now) {
  portENTER_CRITICAL(&powerMux);
  powerState = PowerState();
  powerState.lastActivityMs = now;
  powerState.restStartedMs = now;
  portEXIT_CRITICAL(&powerMux);
  lastKeyMs = now;
}
void noteActivity() {
  // Sólo desde eventos PS/2 completos y válidos. No desde BLE/LED/OTA/keep-alive.
  portENTER_CRITICAL(&powerMux);
  const uint32_t now = millis();
  if (!powerState.awake) powerState.pulseRequested = true;
  powerState.lastActivityMs = now;
  powerState.awake = true;
  portEXIT_CRITICAL(&powerMux);
  lastKeyMs = now;
}
void expireActivity(uint32_t now) { // llamar con powerMux adquirido
  if (powerState.awake && uint32_t(now - powerState.lastActivityMs) >= IDLE_TIMEOUT_MS)
    powerState.awake = false; // queda enclavado hasta una tecla, incluso tras otro wrap
}
bool activityWindowOpen() {
  portENTER_CRITICAL(&powerMux);
  expireActivity(millis());
  const bool active = powerState.awake;
  portEXIT_CRITICAL(&powerMux);
  return active;
}
PowerState powerSnapshot() {
  portENTER_CRITICAL(&powerMux);
  const PowerState copy = powerState;
  portEXIT_CRITICAL(&powerMux);
  return copy;
}
void setPowerForOTA(bool enabled) {
  portENTER_CRITICAL(&powerMux);
  if (enabled && !powerState.otaHold) powerState.pulseRequested = true;
  powerState.otaHold = enabled;
  portEXIT_CRITICAL(&powerMux);
}
void setPowerCommandPause(bool paused) {
  portENTER_CRITICAL(&powerMux);
  powerState.commandPause = paused;
  portEXIT_CRITICAL(&powerMux);
}
bool powerShouldBurn() {
  portENTER_CRITICAL(&powerMux);
  const uint32_t now = millis(); // leer dentro del bloqueo: evita marcas de tiempo cruzadas
  expireActivity(now);
  const bool needed = powerState.otaHold || (powerState.enabled && powerState.awake);
  if (!needed) {
    powerState.active = false;
    powerState.pulseRequested = false;
    powerState.restStartedMs = now;
  } else {
    if (!powerState.active && (powerState.pulseRequested ||
        uint32_t(now - powerState.restStartedMs) >= KEEP_INTERVAL_MS)) {
      powerState.active = true;
      powerState.pulseRequested = false;
      powerState.windowStartedMs = now;
      ++powerState.pulses;
    }
    if (powerState.active && uint32_t(now - powerState.windowStartedMs) >= KEEP_DURATION_MS) {
      powerState.active = false;
      powerState.restStartedMs = now;
    }
  }
  const bool burn = needed && powerState.active && !powerState.commandPause;
  portEXIT_CRITICAL(&powerMux);
  return burn;
}
void cpuKeepAliveSlice() {
  if (!powerShouldBurn()) return;
  const uint32_t start = micros();
  uint32_t value = keepCalculation;
  while (uint32_t(micros() - start) < KEEP_SLICE_US) {
    value = value * 1664525UL + 1013904223UL;
    value ^= value >> 13;
  }
  keepCalculation = value;
}
void powerTask(void *) {
  for (;;) {
    cpuKeepAliveSlice();
    // Prioridad MENOR que loop; nunca bloquea los bits de los comandos PS/2.
    // Ceder siempre permite ejecutar radio, temporizadores e idle.
    vTaskDelay(1);
  }
}

// ================= COLA Y ESTADO HID =================
void clearHIDQueue() {
  memset(hidHeld, 0, sizeof(hidHeld));
  mediaHeld = 0;
  hidR = hidW;
  hidNeedsRelease = false;
  hidRolloverPending = false;
}
void clearPhysicalKeys() {
  memset(ps2Held, 0, sizeof(ps2Held));
  physicalHeldCount = 0;
  otaOwnsF12 = false;
  otaChordTracking = false;
}
void addHID(uint16_t usage, bool down, bool media) {
  if (!usage || otaMode || !readyPrev || !keyboard.isConnected() || !keyboard.isPaired()) return;
  const uint16_t next = (hidW + 1) & HID_MASK;
  if (next == hidR) { ++hidDrops; hidNeedsRelease = true; return; }
  hidEvents[hidW] = {usage, millis(), down, media};
  hidW = next;
  const uint16_t pending = (hidW - hidR) & HID_MASK;
  if (pending > maxQueue) maxQueue = pending;
}
void enqueuePhysicalSnapshot() {
  // Primero modificadores, luego teclas: preserva Ctrl/Shift/Alt al reconectar.
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (uint16_t i = 0; i < 512; ++i) {
      if (!ps2Held[i] || (i == 0x07 && otaOwnsF12)) continue;
      const uint8_t code = static_cast<uint8_t>(i);
      const bool ext = i >= 256;
      const uint8_t key = keyUsage(code, ext);
      if (key && ((key >= 0xE0 && key <= 0xE7) == (pass == 0))) addHID(key, true, false);
      if (pass == 1) {
        const uint16_t media = mediaUsage(code, ext);
        if (media) addHID(media, true, true);
      }
    }
  }
}
void rebuildHIDFromPhysical() {
  if (readyPrev) keyboard.releaseAll();
  clearHIDQueue();
  lastReportAttemptMs = millis();
  lastSendMs = lastReportAttemptMs;
  if (readyPrev) enqueuePhysicalSnapshot();
}
void resetFrame() { bitIndex = 0; frameByte = 0; parityBits = 0; }
void resetParser() { prefixE0 = false; prefixF0 = false; pauseIndex = 0; }
void syncAfterPS2Error() {
  resetFrame(); resetParser(); clearPhysicalKeys();
  // No reconstruir un estado físico que ya puede estar incompleto.
  hidNeedsRelease = true;
  ++resyncs;
}


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


// ================= SINCRONIZACION LED (V2) =================
bool electricalBusIdle(uint32_t quietUs);
bool sendPS2Byte(uint8_t value);

constexpr uint32_t LED_HOST_POLL_MS = 250;
constexpr uint32_t LED_COALESCE_MS = 30;
constexpr uint32_t LED_HELD_GRACE_MS = 120;
constexpr uint32_t LED_PARAMETER_QUIET_US = 3000;
constexpr uint8_t LED_MAX_BYTE_RESENDS = 2;
constexpr uint32_t LED_RECOVERY_DELAYS_MS[] = {100, 300, 1000, 10000};

// ledTarget es SIEMPRE la instantanea de la transaccion iniciada.
// Nunca se reemplaza desde el callback ni mientras se espera un ACK.
uint8_t ledDesired = 0;
portMUX_TYPE ledHostMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t ledHostRawMailbox = 0;
volatile bool ledHostMailboxPending = false;
volatile uint32_t ledHostMailboxReports = 0;
bool ledHostCallbackSeen = false;
uint32_t ledHostReports = 0, ledHostChanges = 0, ledPollChanges = 0;
uint32_t ledLastHostPollMs = 0;

bool ledPendingTracked = false;
uint32_t ledPendingSinceMs = 0;
bool ledUpdateInFlight = false;
bool ledRecoveryRequired = false;
uint8_t ledFailureStreak = 0;
uint32_t ledRecoveryStartedMs = 0, ledRecoveryDelayMs = 0;
CmdState ledRetryResumeState = CMD_IDLE;
uint8_t ledLastCommandByte = 0, ledByteResends = 0;
uint32_t ledResendRequests = 0, ledResends = 0, ledBusyDeferrals = 0;
uint32_t ledTimeouts = 0, ledWriteFailures = 0, ledRecoveries = 0;
uint32_t ledUnexpectedResponses = 0, ledConfirmedUpdates = 0;
uint32_t ledKeyboardResets = 0;
char ledLastError[72] = "ninguno";

uint8_t ledHIDToPS2(uint8_t hidMask) {
  return uint8_t(((hidMask & 0x01u) << 1) |
                 ((hidMask & 0x02u) << 1) |
                 ((hidMask & 0x04u) >> 2));
}

// Contexto NimBLE: publicar y salir. Nada de PS/2, Serial, delay o asignacion.
void ledChangedFromHost(uint8_t rawHidMask) {
  portENTER_CRITICAL(&ledHostMux);
  ledHostRawMailbox = rawHidMask & 0x07u;
  ledHostMailboxPending = true;
  ++ledHostMailboxReports;
  portEXIT_CRITICAL(&ledHostMux);
}

uint8_t sampleHostLEDMask() {
  uint8_t value = 0;
  if (keyboard.isScrollLockOn()) value |= 1;
  if (keyboard.isNumLockOn()) value |= 2;
  if (keyboard.isCapsLockOn()) value |= 4;
  return value;
}

uint8_t hostLEDMask() { return ledDesired; }

void ledTrackPending(uint32_t now) {
  if (!ledPendingTracked) {
    ledPendingTracked = true;
    ledPendingSinceMs = now;
  }
}

void ledApplyHostMask(uint8_t mask, uint32_t now, bool fromPoll) {
  mask &= 0x07u;
  if (mask == ledDesired) return;
  ledDesired = mask;
  lastObservedLED = mask;
  ledObservedAt = now;
  ++ledHostChanges;
  if (fromPoll) ++ledPollChanges;
  if (autoLED && (!ledKnown || ledDesired != ledConfirmed)) ledTrackPending(now);
}

void serviceHostLEDState() {
  if (!readyPrev || otaMode) return;
  const uint32_t now = millis();
  uint8_t raw = 0;
  bool pending = false;
  portENTER_CRITICAL(&ledHostMux);
  pending = ledHostMailboxPending;
  if (pending) {
    raw = ledHostRawMailbox;
    ledHostMailboxPending = false;
  }
  ledHostReports = ledHostMailboxReports;
  portEXIT_CRITICAL(&ledHostMux);
  if (pending) {
    ledHostCallbackSeen = true;
    ledLastHostPollMs = now;
    ledApplyHostMask(ledHIDToPS2(raw), now, false);
    return;
  }
  if (uint32_t(now - ledLastHostPollMs) < LED_HOST_POLL_MS) return;
  ledLastHostPollMs = now;
  const uint8_t sampled = sampleHostLEDMask();
  // Si el callback se publico durante el muestreo, tiene precedencia.
  portENTER_CRITICAL(&ledHostMux);
  pending = ledHostMailboxPending;
  if (pending) {
    raw = ledHostRawMailbox;
    ledHostMailboxPending = false;
  }
  ledHostReports = ledHostMailboxReports;
  portEXIT_CRITICAL(&ledHostMux);
  if (pending) {
    ledHostCallbackSeen = true;
    ledApplyHostMask(ledHIDToPS2(raw), now, false);
  } else {
    ledApplyHostMask(sampled, now, true);
  }
}

void resetLEDConnection(bool ready) {
  const uint32_t now = millis();
  readyPrev = ready;
  echoOK = false;
  ledKnown = false;
  wantLED = false;
  ledHostCallbackSeen = false;
  ledLastHostPollMs = now;
  if (ready) {
    connectedAt = now;
    ledDesired = sampleHostLEDMask();
    lastObservedLED = ledDesired;
    ledObservedAt = now;
    // No borrar un callback que haya llegado justo antes del evento READY.
    wantEcho = ledRecoveryRequired;
    if (autoLED) ledTrackPending(now);
  } else {
    portENTER_CRITICAL(&ledHostMux);
    ledHostMailboxPending = false;
    portEXIT_CRITICAL(&ledHostMux);
    wantEcho = ledRecoveryRequired;
    if (cmdState == CMD_IDLE && !ledRecoveryRequired) ledPendingTracked = false;
  }
  // Una desconexion BLE NO cancela ED/mascara. serviceCommands lo completa.
}

bool ledParserBoundaryClear() {
  return !prefixE0 && !prefixF0 && pauseIndex == 0;
}

bool readyForNewCommand() {
  if (!ledParserBoundaryClear() || hidR != hidW ||
      !electricalBusIdle(PS2_TX_QUIET_US)) return false;
  return physicalHeldCount == 0 ||
      (ledPendingTracked &&
       uint32_t(millis() - ledPendingSinceMs) >= LED_HELD_GRACE_MS);
}

bool ledReadyForParameter() {
  return ledParserBoundaryClear() &&
         electricalBusIdle(LED_PARAMETER_QUIET_US);
}

bool ledRecoveryPendingForOTA() {
  return cmdState != CMD_IDLE || ledRecoveryRequired;
}

void commandError(const char *why) {
  ++txErrors;
  if (ledUpdateInFlight) wantLED = true; // conservar tambien un pedido manual
  ledUpdateInFlight = false;
  cmdState = CMD_IDLE;
  ledRetryResumeState = CMD_IDLE;
  ledByteResends = 0;
  ledKnown = false;
  echoOK = false;
  // Los errores temporales nunca apagan AUTO ni dejan un bloqueo permanente.
  txBlocked = false;
  ledRecoveryRequired = true;
  wantEcho = true;
  const uint32_t now = millis();
  ledRecoveryStartedMs = now;
  if (ledFailureStreak < 4) ++ledFailureStreak;
  ledRecoveryDelayMs = LED_RECOVERY_DELAYS_MS[ledFailureStreak - 1];
  ledTrackPending(now);
  snprintf(ledLastError, sizeof(ledLastError), "%s", why ? why : "error PS/2");
  logf("[LED] Recuperacion: %s; ECHO en %lu ms (fallos=%u).\n",
       ledLastError, (unsigned long)ledRecoveryDelayMs, ledFailureStreak);
}

// BAT invalida la sincronizacion; el receptor se recupera por su propio camino.
void ledKeyboardReset() {
  ++ledKeyboardResets;
  cmdState = CMD_IDLE;
  ledRetryResumeState = CMD_IDLE;
  ledByteResends = 0;
  ledUpdateInFlight = false;
  ledKnown = false;
  echoOK = false;
  wantLED = false;
  wantEcho = true;
  txBlocked = false;
  ledFailureStreak = 0;
  ledRecoveryRequired = true;
  ledRecoveryStartedMs = millis();
  ledRecoveryDelayMs = LED_RECOVERY_DELAYS_MS[0];
  ledTrackPending(ledRecoveryStartedMs);
}

bool ledSendAndWait(uint8_t value, CmdState awaiting, bool resend,
                    const char *failureMessage) {
  if (!sendPS2Byte(value)) {
    if (ps2TxAttempted) {
      ++ledWriteFailures;
      commandError(failureMessage);
    } else {
      ++ledBusyDeferrals;
    }
    return false;
  }
  ledLastCommandByte = value;
  ledRetryResumeState = awaiting;
  if (resend) {
    ++ledByteResends;
    ++ledResends;
  } else {
    ledByteResends = 0;
  }
  cmdState = awaiting;
  commandStartedAt = millis();
  return true;
}

bool consumeCommandAnswer(uint8_t b) {
  if (b == 0xEE && cmdState == CMD_WAIT_ECHO) {
    const bool recovered = ledRecoveryRequired;
    cmdState = CMD_IDLE;
    ledRetryResumeState = CMD_IDLE;
    ledByteResends = 0;
    echoOK = true;
    ledKnown = false;
    ledRecoveryRequired = false;
    ledRecoveryDelayMs = 0;
    ++txOK;
    if (recovered) ++ledRecoveries;
    if (!readyPrev) ledFailureStreak = 0;
    if (readyPrev && (autoLED || wantLED)) ledTrackPending(millis());
    else ledPendingTracked = false;
    logLine(recovered ? "[LED] ECHO correcto; enlace PS/2 recuperado." :
                        "[TX] ECHO 0xEE correcto.");
    return true;
  }
  if (b == 0xFA && cmdState == CMD_WAIT_ED_ACK) {
    cmdState = CMD_SEND_MASK;
    ledRetryResumeState = CMD_IDLE;
    ledByteResends = 0;
    commandStartedAt = millis();
    return true;
  }
  if (b == 0xFA && cmdState == CMD_WAIT_MASK_ACK) {
    cmdState = CMD_IDLE;
    ledRetryResumeState = CMD_IDLE;
    ledByteResends = 0;
    ledUpdateInFlight = false;
    ledConfirmed = ledTarget; // ACK de lo enviado, NO del target nuevo del host
    ledKnown = true;
    ledFailureStreak = 0;
    ++txOK;
    ++ledConfirmedUpdates;
    if (readyPrev && (wantLED || (autoLED && ledDesired != ledConfirmed))) {
      ledTrackPending(millis());
    } else {
      ledPendingTracked = false;
    }
    logf("[LED] Confirmado=%02X deseado=%02X Caps=%u Num=%u Scroll=%u.\n",
         ledConfirmed, ledDesired, !!(ledConfirmed & 4),
         !!(ledConfirmed & 2), !!(ledConfirmed & 1));
    return true;
  }
  if (b == 0xFE) {
    ++ledResendRequests;
    if (cmdState == CMD_WAIT_ECHO || cmdState == CMD_WAIT_ED_ACK ||
        cmdState == CMD_WAIT_MASK_ACK) {
      if (ledByteResends >= LED_MAX_BYTE_RESENDS) {
        commandError("RESEND: limite de reenvios del byte");
      } else {
        ledRetryResumeState = cmdState;
        cmdState = CMD_RETRY_BYTE;
        commandStartedAt = millis();
      }
    } else if (cmdState == CMD_SEND_MASK) {
      commandError("RESEND fuera de fase tras ACK de 0xED");
    } else {
      // Un FE duplicado antes del reenvio no consume otro intento.
      ++ledUnexpectedResponses;
    }
    return true;
  }
  if (b == 0xFA || b == 0xEE) {
    ++ledUnexpectedResponses; // incluye ACK tardios durante recuperacion ECHO
    return true;
  }
  return false;
}

void serviceCommands() {
  if (otaMode || txBlocked) return;
  const uint32_t now = millis();

  // Resolver todas las fases en vuelo, aun sin BLE o con OTA solicitada.
  if (cmdState == CMD_WAIT_ECHO || cmdState == CMD_WAIT_ED_ACK ||
      cmdState == CMD_WAIT_MASK_ACK) {
    if (uint32_t(now - commandStartedAt) >= CMD_TIMEOUT_MS) {
      ++ledTimeouts;
      commandError("sin respuesta al comando PS/2");
    }
    return;
  }
  if (cmdState == CMD_RETRY_BYTE) {
    if (uint32_t(now - commandStartedAt) >= LED_PARAM_TIMEOUT_MS) {
      ++ledTimeouts;
      commandError("no fue posible reenviar el byte");
      return;
    }
    if (!ledReadyForParameter()) return;
    ledSendAndWait(ledLastCommandByte, ledRetryResumeState, true,
                   "fallo electrico al reenviar byte");
    return;
  }
  if (cmdState == CMD_SEND_MASK) {
    if (uint32_t(now - commandStartedAt) >= LED_PARAM_TIMEOUT_MS) {
      ++ledTimeouts;
      commandError("no fue posible completar 0xED");
      return;
    }
    if (!ledReadyForParameter()) return;
    ledSendAndWait(ledTarget, CMD_WAIT_MASK_ACK, false,
                   "fallo electrico al enviar mascara LED");
    return;
  }

  // Una operacion ambigua se cancela con ECHO: una mascara NO se reenvia
  // ciegamente por timeout. FE explicito si permite reenviar el mismo byte.
  if (ledRecoveryRequired) {
    if (uint32_t(now - ledRecoveryStartedMs) < ledRecoveryDelayMs) return;
    ledTrackPending(now);
    if (!readyForNewCommand()) return;
    if (ledSendAndWait(0xEE, CMD_WAIT_ECHO, false,
                      "fallo electrico al recuperar con ECHO")) wantEcho = false;
    return;
  }

  // Nuevas sincronizaciones normales requieren BLE; la recuperacion anterior no.
  if (otaRequested || !readyPrev) return;
  if (!echoOK && !wantEcho &&
      uint32_t(now - connectedAt) >= BLE_SETTLE_MS) wantEcho = true;
  if (wantEcho) {
    ledTrackPending(now);
    if (!readyForNewCommand()) return;
    if (ledSendAndWait(0xEE, CMD_WAIT_ECHO, false,
                      "fallo electrico al enviar ECHO")) wantEcho = false;
    return;
  }
  if (!echoOK || (!autoLED && !wantLED)) return;
  const bool dirty = !ledKnown || ledDesired != ledConfirmed;
  if (autoLED && dirty) {
    ledTrackPending(now);
    if (uint32_t(now - ledObservedAt) >= LED_COALESCE_MS) wantLED = true;
  }
  if (!wantLED) {
    if (!dirty) ledPendingTracked = false;
    return;
  }
  ledTrackPending(now);
  if (!readyForNewCommand()) return;
  ledTarget = ledDesired;
  ledUpdateInFlight = true;
  if (ledSendAndWait(0xED, CMD_WAIT_ED_ACK, false,
                     "fallo electrico al enviar 0xED")) {
    wantLED = false;
  } else if (!ps2TxAttempted) {
    ledUpdateInFlight = false; // todavia no comenzo la transaccion
  }
}

void printLEDStats() {
  const uint32_t age = ledPendingTracked ? uint32_t(millis() - ledPendingSinceMs) : 0;
  logf("[LED] HOST=%02X EN_VUELO=%02X CONF=%02X KNOWN=%u AUTO=%u ECHO=%u STATE=%u PEND_MS=%lu\n",
       ledDesired, ledTarget, ledConfirmed, ledKnown, autoLED, echoOK,
       unsigned(cmdState), (unsigned long)age);
  logf("[LED] HOST_REPORTS=%lu CHANGES=%lu POLL_CHANGES=%lu CALLBACK=%u OK=%lu ERR=%lu\n",
       (unsigned long)ledHostReports, (unsigned long)ledHostChanges,
       (unsigned long)ledPollChanges, ledHostCallbackSeen,
       (unsigned long)ledConfirmedUpdates, (unsigned long)txErrors);
  logf("[LED] FE=%lu REENVIOS=%lu TIMEOUT=%lu TX_FAIL=%lu BUSY=%lu RECUP=%lu BAT=%lu EXTRA=%lu\n",
       (unsigned long)ledResendRequests, (unsigned long)ledResends,
       (unsigned long)ledTimeouts, (unsigned long)ledWriteFailures,
       (unsigned long)ledBusyDeferrals, (unsigned long)ledRecoveries,
       (unsigned long)ledKeyboardResets, (unsigned long)ledUnexpectedResponses);
  logf("[LED] RECOVERY=%u STREAK=%u COOLDOWN=%lu LAST_BYTE=%02X RETRY=%u LAST_ERROR=%s\n",
       ledRecoveryRequired, ledFailureStreak, (unsigned long)ledRecoveryDelayMs,
       ledLastCommandByte, ledByteResends, ledLastError);
}


// ============== DECODIFICADOR PS/2 ==============
bool ctrlPhysicallyDown() { return ps2Held[0x14] || ps2Held[256 + 0x14]; }
bool altPhysicallyDown() { return ps2Held[0x11] || ps2Held[256 + 0x11]; }
void processKey(uint8_t c, bool ext, bool down) {
  if (ext && c == 0x12) return; // Shift sintético de PrintScreen
  const uint16_t media = mediaUsage(c, ext);
  const uint8_t key = keyUsage(c, ext);
  if (!media && !key) {
    ++unmapped;
    if (debugKeys) logf("[PS2] Sin mapa %s%02X\n", ext ? "E0 " : "", c);
    return;
  }
  noteActivity(); // contar también typematic aunque no se duplique en HID
  if (down) ++makes; else ++breaks;
  const uint16_t index = c + (ext ? 256 : 0);
  const bool wasDown = ps2Held[index];
  if (down != wasDown) {
    ps2Held[index] = down;
    if (down) ++physicalHeldCount;
    else if (physicalHeldCount) --physicalHeldCount;
  }
  if (!ext && c == 0x07) {
    if (down && !wasDown && ctrlPhysicallyDown() && altPhysicallyDown()) otaOwnsF12 = true;
    if (otaOwnsF12) {
      if (!down) otaOwnsF12 = false;
      return; // combinación reservada: no enviar F12 al sistema operativo
    }
  }
  if (down == wasDown) {
    ++ignored;
    if (down) ++duplicateMakes;
    return;
  }
  addHID(media ? media : key, down, media != 0);
  if (debugKeys) logf("[KEY] %s %s%02X\n", down ? "MAKE" : "BREAK", ext ? "E0 " : "", c);
}
void processPS2Byte(uint8_t b, uint32_t receivedUs) {
  if (haveDecodedByte && uint32_t(receivedUs - lastDecodedByteUs) > 100000) resetParser();
  lastDecodedByteUs = receivedUs;
  haveDecodedByte = true;
  if (b == 0x00 || b == 0xFF) { syncAfterPS2Error(); return; }
  if (b == 0xAA) { // BAT: el teclado físico reinició; no es una tecla.
    syncAfterPS2Error();
    ledKeyboardReset();
    return;
  }
  if (consumeCommandAnswer(b)) return;
  if (pauseIndex || b == 0xE1) {
    if (b == PAUSE_SEQ[pauseIndex]) {
      if (++pauseIndex == 8) {
        pauseIndex = 0; prefixE0 = false; prefixF0 = false;
        ++pauses; noteActivity();
        addHID(0x48, true, false); addHID(0x48, false, false);
      }
    } else resetParser();
    return;
  }
  if (b == 0xE0) { prefixE0 = true; return; }
  if (b == 0xF0) { prefixF0 = true; return; }
  const bool ext = prefixE0, down = !prefixF0;
  prefixE0 = false; prefixF0 = false;
  processKey(b, ext, down);
}
void processFall(const PS2Edge &edge) {
  if (haveFall && uint32_t(edge.timeUs - lastFallUs) > 300 && bitIndex != 0) {
    ++cutFrames; syncAfterPS2Error();
  }
  lastFallUs = edge.timeUs; haveFall = true;
  const uint8_t bit = edge.data & 1;
  if (bitIndex == 0) {
    if (bit) { ++badStart; return; }
    frameByte = 0; parityBits = 0;
  } else if (bitIndex <= 8) {
    frameByte |= uint8_t(bit << (bitIndex - 1)); parityBits ^= bit;
  } else if (bitIndex == 9) parityBits ^= bit;
  if (++bitIndex == 11) {
    if (!bit) { ++badStop; syncAfterPS2Error(); }
    else if (parityBits != 1) { ++badParity; syncAfterPS2Error(); }
    else { ++bytesOK; processPS2Byte(frameByte, edge.timeUs); }
    resetFrame();
  }
}
void servicePS2() {
  uint32_t drops;
  noInterrupts(); drops = edgeDrops; interrupts();
  if (drops != lastDropSeen) {
    lastDropSeen = drops;
    clearEdgeQueue(); syncAfterPS2Error(); haveFall = false;
  }
  PS2Edge edge;
  uint16_t count = 0;
  while (count++ < 512 && popEdge(edge)) processFall(edge);
  // Un ACK cortado puede dejar de generar reloj. No esperar otro byte para
  // soltar el frame incompleto: impediria enviar el ECHO de recuperacion.
  if (edgeR == edgeW && bitIndex != 0 && haveFall &&
      uint32_t(micros() - lastFallUs) > 2000) {
    ++cutFrames;
    syncAfterPS2Error();
    haveFall = false;
  }
  // Un prefijo cortado no puede impedir para siempre los comandos LED.
  // Se usa el tiempo del cable, no el tiempo de vaciado de la cola.
  if (edgeR == edgeW && bitIndex == 0 && haveDecodedByte &&
      uint32_t(micros() - lastDecodedByteUs) > 100000 &&
      (prefixE0 || prefixF0 || pauseIndex)) resetParser();
}


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

bool sendPS2Byte(uint8_t b) {
  ps2TxAttempted = false;
  if (!electricalBusIdle(3000)) {
    return false;
  }
  ps2TxAttempted = true;

  setPowerCommandPause(true);
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
  setPowerCommandPause(false);

  return ok;
}


// ============== CONEXIÓN Y ENVÍO HID ==============
void serviceConnection() {
  if (otaMode) return;
  const bool ready = keyboard.isConnected() && keyboard.isPaired();
  if (ready == readyPrev) return;
  resetLEDConnection(ready);
  if (ready) {
    rebuildHIDFromPhysical();
    logLine("[BLE] Lista; sincronizando estado y LED.");
  } else {
    clearHIDQueue();
    logLine("[BLE] Desconectada; se completa cualquier comando LED pendiente.");
  }
}
unsigned countHidNonModifiers() {
  unsigned n = 0;
  for (unsigned i = 1; i < 0xE0; ++i) if (hidHeld[i]) ++n;
  return n;
}
bool anyHidHeld() {
  if (mediaHeld) return true;
  for (unsigned i = 0; i < 256; ++i) if (hidHeld[i]) return true;
  return false;
}
void restoreRolloverKeys() {
  if (!hidRolloverPending || hidR != hidW || !readyPrev) return;
  unsigned slots = countHidNonModifiers();
  hidRolloverPending = false;
  for (uint16_t i = 0; i < 512; ++i) {
    if (!ps2Held[i] || (i == 7 && otaOwnsF12)) continue;
    const uint8_t key = keyUsage(static_cast<uint8_t>(i), i >= 256);
    if (!key || key >= 0xE0 || hidHeld[key]) continue;
    if (slots < 6) { addHID(key, true, false); ++slots; }
    else hidRolloverPending = true;
  }
}
void restoreHeldMedia() {
  // El descriptor admite un consumidor activo. Si se soltó una segunda tecla
  // multimedia, restaurar la primera aún retenida aunque sus repeats se filtren.
  if (mediaHeld || hidR != hidW || !readyPrev || otaMode) return;
  for (uint16_t i = 256; i < 512; ++i) {
    if (!ps2Held[i]) continue;
    const uint16_t usage = mediaUsage(static_cast<uint8_t>(i), true);
    if (usage) { addHID(usage, true, true); return; }
  }
}
void serviceHID() {
  if (!readyPrev || otaMode) return;
  if (hidNeedsRelease) { rebuildHIDFromPhysical(); return; }
  if (hidR != hidW && uint32_t(millis() - hidEvents[hidR].queuedMs) >= HID_STALE_MS) {
    ++staleRecoveries; rebuildHIDFromPhysical(); return;
  }
  if (uint32_t(millis() - lastSendMs) < hidGapMs) return;
  // Descartar duplicados sin consumir un intervalo entero por cada uno.
  for (unsigned budget = 0; budget < 16 && hidR != hidW; ++budget) {
    const HIDEvent event = hidEvents[hidR];
    hidR = (hidR + 1) & HID_MASK;
    const uint8_t usage = static_cast<uint8_t>(event.usage);
    if (event.media) {
      if ((event.down && mediaHeld == event.usage) || (!event.down && mediaHeld != event.usage)) {
        ++ignored; continue;
      }
    } else {
      if (hidHeld[usage] == event.down) { ++ignored; continue; }
      // El descriptor estándar de HijelHID es 6KRO; no fingir que envió la séptima.
      if (event.down && usage < 0xE0 && countHidNonModifiers() >= 6) {
        hidRolloverPending = true; ++ignored; continue;
      }
    }
    const uint32_t wait = millis() - event.queuedMs;
    if (wait > maxQueueWaitMs) maxQueueWaitMs = wait;
    sumQueueWaitMs += wait; ++queueSamples;
    const uint32_t callStart = micros();
    if (event.media) {
      if (event.down) {
        if (mediaHeld) keyboard.release(static_cast<uint16_t>(mediaHeld));
        keyboard.press(static_cast<uint16_t>(event.usage));
        mediaHeld = event.usage;
      } else {
        keyboard.release(static_cast<uint16_t>(event.usage)); mediaHeld = 0;
      }
    } else {
      if (event.down) keyboard.press(usage); else keyboard.release(usage);
      hidHeld[usage] = event.down;
    }
    const uint32_t callTime = micros() - callStart;
    if (callTime > maxHidCallUs) maxHidCallUs = callTime;
    ++hidSent; // intentos de informe, NO confirmaciones de recepción del anfitrión
    lastSendMs = millis(); lastReportAttemptMs = lastSendMs;
    restoreRolloverKeys();
    restoreHeldMedia();
    return;
  }
  restoreRolloverKeys();
  restoreHeldMedia();
}
void serviceWarmReport() {
  if (!warmReportsEnabled || !readyPrev || otaMode || otaRequested || !activityWindowOpen()) return;
  if (physicalHeldCount || anyHidHeld() || hidR != hidW || hidNeedsRelease ||
      cmdState != CMD_IDLE || !electricalBusIdle(3000)) return;
  if (uint32_t(millis() - lastReportAttemptMs) < BLE_WARM_INTERVAL_MS) return;
  // Informe realmente vacío. Evita el priming de 50 ms tras pausas de la
  // biblioteca sin enviar caracteres ni falsificar actividad del usuario.
  keyboard.releaseAll();
  lastReportAttemptMs = millis();
  ++warmReports;
}

// ================= WATCHDOG =================
bool configureWatchdog(uint32_t timeoutMs) {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = timeoutMs;
  config.idle_core_mask = 0;
  config.trigger_panic = true;
  const esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result != ESP_OK) return false;
  watchdogTimeoutMs = timeoutMs;
  if (watchdogEnabled) esp_task_wdt_reset();
  return true;
}
void setupWatchdog() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = 15000;
  config.idle_core_mask = 0;
  config.trigger_panic = true;
  const esp_err_t status = esp_task_wdt_status(nullptr);
  esp_err_t result = status == ESP_ERR_INVALID_STATE ?
      esp_task_wdt_init(&config) : esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_reconfigure(&config);
  watchdogEnabled = false;
  if (result == ESP_OK) {
    watchdogTimeoutMs = config.timeout_ms;
    if (esp_task_wdt_status(nullptr) == ESP_OK) watchdogEnabled = true;
    else {
      result = esp_task_wdt_add(nullptr);
      watchdogEnabled = result == ESP_OK;
    }
  }
  logf("[WDT] %s, 15 s; resultado=%s\n",
       watchdogEnabled ? "activo" : "no disponible (OTA bloqueada)",
       esp_err_to_name(result));
}
void feedWatchdog() { if (watchdogEnabled) esp_task_wdt_reset(); }

// ================= OTA BAJO DEMANDA =================
// Verificar que begin() realmente dejo un socket escuchando, sin abrir
// una conexion de prueba que pueda ocupar el unico cliente HTTP del servidor.
class KeyboardWebServer : public WebServer {
public:
  KeyboardWebServer() : WebServer(80) {}
  bool listening() { return static_cast<bool>(_server); }
};
KeyboardWebServer otaWeb;

bool otaBootMarkerCleared = false;

bool otaPartitionsValid() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *data = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  const esp_partition_t *a = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t *b = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  return run && boot && target && data && data->size >= 0x2000 &&
      a && a->size && b && b->size && target->size &&
      target->address != run->address && boot->address == run->address;
}

bool consumeOTABootRequest() {
  // La primera instalacion desde cero abre la web una sola vez. Persistir
  // la marca ANTES de abrir Wi-Fi evita bucles OTA ante un fallo de arranque.
  otaBootMarkerCleared = false;
  Preferences prefs;
  if (!prefs.begin("kb9970", false)) {
    logLine("[OTA] No se pudo abrir NVS; arranque normal.");
    return false;
  }
  const bool requested = prefs.getBool("ota", false);
  // Mantener la clave NVS "web72" por compatibilidad con instalaciones previas.
  // Renombrarla haria que OTA se abra automaticamente tras actualizar desde V7.2.
  const bool firstInstall = !prefs.getBool("web72", false);
  bool cleared = true;
  if (requested) {
    cleared = prefs.putBool("ota", false) == 1;
    if (cleared) cleared = !prefs.getBool("ota", true);
  }
  bool firstMarked = false;
  if (cleared && firstInstall) {
    firstMarked = prefs.putBool("web72", true) == 1;
    if (firstMarked) firstMarked = prefs.getBool("web72", false);
  }
  prefs.end();
  if (!cleared) {
    logLine("[OTA] No se pudo consumir la solicitud; arranque normal.");
    return false;
  }
  otaBootMarkerCleared = requested || firstMarked;
  if (firstMarked) logLine("[WEB] Primera instalacion del firmware web: mantenimiento automatico15min.");
  else if (firstInstall && !requested) logLine("[WEB] No se pudo guardar marca inicial; usar atajo para abrir web.");
  return otaBootMarkerCleared;
}

bool persistOTABootRequest() {
  Preferences prefs;
  if (!prefs.begin("kb9970", false)) return false;
  bool stored = prefs.putBool("ota", true) == 1;
  if (stored) stored = prefs.getBool("ota", false);
  // Si la verificación falla, intentar retirar la solicitud ambigua.
  if (!stored) prefs.putBool("ota", false);
  prefs.end();
  return stored;
}

void resetOtaChord(bool needRelease) {
  otaChordTracking = false;
  otaChordFired = false;
  otaChordNeedsRelease = needRelease;
}

void requestOTA() {
  if (otaMode || otaRequested) return;
  if (!powerTaskRunning || !watchdogEnabled) {
    logLine("[OTA] Requiere tarea de alimentacion y watchdog activos.");
    return;
  }
  if (!otaPartitionsValid()) {
    logLine("[OTA] Particiones incompatibles: abrir web de diagnostico, sin permitir carga.");
  }
  if (!strcmp(OTA_AP_PASSWORD, OTA_UPLOAD_PASSWORD)) {
    logLine("[OTA] Las claves Wi-Fi y OTA deben ser distintas.");
    return;
  }
  otaRequested = true;
  otaRequestMs = millis();
  logLine("[OTA] Soltar todas las teclas; esperando bus libre para reiniciar.");
}

void serviceOtaShortcut() {
  if (otaMode) return;
  const bool chord = ctrlPhysicallyDown() && altPhysicallyDown() &&
      ps2Held[7] && otaOwnsF12;
  if (!chord) { resetOtaChord(false); return; }
  if (otaChordNeedsRelease || otaChordFired) return;
  if (!otaChordTracking) {
    otaChordTracking = true;
    otaChordStartMs = millis();
  }
  if (uint32_t(millis() - otaChordStartMs) >= OTA_HOLD_MS) {
    otaChordFired = true;
    requestOTA();
  }
}

void scheduleOtaReboot(const char *why) {
  otaRebootPending = true;
  otaRestartMs = millis();
  setPowerForOTA(true); // mantener keep-alive hasta ESP.restart()
  logLine(why);
}

void finishOtaExit() {
  // Se ejecuta fuera de los callbacks HTTP. No cerrar el servidor durante la carga.
  if (otaBusy || otaRebootPending) return;
  if (otaListening) { otaWeb.close(); otaListening = false; }
  if (Update.isRunning()) Update.abort();
  if (!WiFi.mode(WIFI_OFF)) {
    otaClosing = true;
    wifiStopAttemptMs = millis();
    if (++wifiStopAttempts >= 3) {
      scheduleOtaReboot("[OTA] Wi-Fi no responde al cierre; reinicio normal.");
    }
    return;
  }
  otaClosing = false;
  // El flag NVS se consumió antes de abrir el AP: vuelve a teclado.
  scheduleOtaReboot("[OTA] Fin de mantenimiento; reinicio al modo teclado.");
}

void stopOTA() {
  if (!otaMode || otaBusy || otaRebootPending || otaClosing) return;
  otaClosing = true;
  finishOtaExit();
}

void beginOTA() {
  // Se llama en modo teclado para preparar EL SIGUIENTE arranque.
  if (!otaRequested || otaMode) return;
  if (cmdState != CMD_IDLE || physicalHeldCount || hidR != hidW ||
      !electricalBusIdle(PS2_TX_QUIET_US)) return;
  // Dar tiempo al ECHO de recuperacion, sin bloquear mantenimiento para
  // siempre si PS/2 esta averiado. Nunca cortar una fase que sigue en vuelo.
  if (ledRecoveryRequired &&
      uint32_t(millis() - otaRequestMs) < OTA_LED_RECOVERY_GRACE_MS) return;
  if (!persistOTABootRequest()) {
    otaRequested = false;
    resetOtaChord(true);
    logLine("[OTA] No se pudo guardar solicitud; el teclado sigue activo.");
    return;
  }
  if (readyPrev) keyboard.releaseAll();
  if (ledRecoveryRequired) {
    logLine("[OTA] Bus libre; recuperacion LED diferida hasta volver al teclado.");
  }
  clearHIDQueue();
  if (ps2ReceiverAttached) {
    detachInterrupt(digitalPinToInterrupt(PIN_CLOCK));
    ps2ReceiverAttached = false;
  }
  pinMode(PIN_DATA, INPUT_PULLUP);
  pinMode(PIN_CLOCK, INPUT_PULLUP);
  otaRequested = false;
  otaMode = true; // loop sólo drena diagnóstico y atiende el reinicio pendiente
  scheduleOtaReboot("[OTA] Solicitud guardada; reinicio exclusivo Wi-Fi.");
}

bool startOTABootMode() {
  // Invocar únicamente al arrancar y ANTES de keyboard.begin()/ISR PS2.
  if (!otaBootMarkerCleared) return false;
  if (!powerTaskRunning || !watchdogEnabled ||
      !strcmp(OTA_AP_PASSWORD, OTA_UPLOAD_PASSWORD)) {
    logLine("[OTA] Condiciones de mantenimiento incompletas; arranque normal.");
    return false;
  }
  if (!configureWatchdog(60000)) {
    logLine("[OTA] No se pudo ampliar watchdog; arranque normal.");
    return false;
  }
  otaMode = true;
  otaRequested = false;
  otaBusy = false;
  otaRejected = false;
  otaErrorPending = false;
  otaRebootPending = false;
  otaClosing = false;
  wifiStopAttempts = 0;
  otaProgress = 0;
  lastOtaError = -1;
  setPowerForOTA(true);
  pinMode(PIN_DATA, INPUT_PULLUP);
  pinMode(PIN_CLOCK, INPUT_PULLUP);
  logLine("[OTA] Arranque exclusivo Wi-Fi; Bluetooth no se inicializa.");
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (target && otaPartitionsValid()) {
    logf("[OTA] Destino=%s espacio=%lu bytes\n", target->label,
         static_cast<unsigned long>(target->size));
  } else {
    logLine("[OTA] Sin slot compatible: pagina disponible para diagnostico.");
  }

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_OFF)) {
    scheduleOtaReboot("[OTA] Fallo al preparar radio apagada; reinicio normal.");
    return true;
  }
  delay(150);
  feedWatchdog();
  if (!WiFi.mode(WIFI_AP)) {
    logLine("[OTA] No se pudo iniciar WIFI_AP.");
    stopOTA();
    return true;
  }
  delay(150);
  const bool beforePower = WiFi.setTxPower(WIFI_POWER_8_5dBm);
  logf("[RADIO] Limite TX previo 8.5 dBm: %s\n", beforePower ? "OK" : "pendiente");

  const IPAddress local(192, 168, 4, 1), mask(255, 255, 255, 0);
  const bool apConfigured = WiFi.softAPConfig(local, local, mask);
  const bool apStarted = apConfigured &&
      WiFi.softAP(OTA_AP_NAME, OTA_AP_PASSWORD, 1, 0, 2);
  delay(400);
  feedWatchdog();
  const IPAddress ip = WiFi.softAPIP();
  logf("[OTA] softAP=%s IP=%s canal=1\n", apStarted ? "OK" : "FALLO",
       ip.toString().c_str());
  if (!apStarted || ip != local) {
    logLine("[OTA] AP no disponible; cerrando mantenimiento.");
    stopOTA();
    return true;
  }

  // Refuerzo después de iniciar el AP; conserva el ajuste probado en V6.1.
  esp_err_t powerResult = ESP_FAIL;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    powerResult = esp_wifi_set_max_tx_power(34);
    if (powerResult == ESP_OK) break;
    delay(50);
    feedWatchdog();
  }
  logf("[RADIO] Solicitud TX raw=34: %s\n", esp_err_to_name(powerResult));
  int8_t actualPower = 0;
  const esp_err_t readResult = esp_wifi_get_max_tx_power(&actualPower);
  if (readResult == ESP_OK) {
    logf("[RADIO] Limite leido: raw=%d (%.2f dBm)\n",
         static_cast<int>(actualPower), actualPower / 4.0);
  } else {
    logf("[RADIO] Lectura TX: %s\n", esp_err_to_name(readResult));
  }
  if (powerResult != ESP_OK) {
    logLine("[OTA] No se pudo aplicar limite Wi-Fi probado; cerrando.");
    stopOTA();
    return true;
  }
  wifi_config_t apConfig = {};
  const esp_err_t configResult = esp_wifi_get_config(WIFI_IF_AP, &apConfig);
  if (configResult == ESP_OK) {
    int length = static_cast<int>(apConfig.ap.ssid_len);
    if (length == 0 || length > 32) length = 32;
    logf("[RADIO] SSID=%.*s oculto=%u canal=%u auth=%u\n", length,
         reinterpret_cast<const char *>(apConfig.ap.ssid),
         static_cast<unsigned>(apConfig.ap.ssid_hidden),
         static_cast<unsigned>(apConfig.ap.channel),
         static_cast<unsigned>(apConfig.ap.authmode));
  } else {
    logf("[RADIO] Lectura AP: %s\n", esp_err_to_name(configResult));
  }
  logf("[RADIO] ESP-IDF=%s\n", esp_get_idf_version());
  configureWebOTA();
  otaListening = false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    otaWeb.begin();
    if (otaWeb.listening()) { otaListening = true; break; }
    feedWatchdog();
    delay(50);
  }
  if (!otaListening) {
    logLine("[WEB] No se pudo abrir puerto80; reinicio normal.");
    stopOTA();
    return true;
  }
  otaStartMs = millis(); // GET/status/errores no prolongan esta ventana fija.
  logLine("[WEB] V2.0.0 LISTA: http://192.168.4.1/ RED HP-KB9970-WEB claveWiFi teclado1.");
  logLine("[WEB] Pagina publica; clave para cargar: teclado. Ventana15min.");
  return true;
}

const uint8_t OTA_WEB_PAGE_GZ[] PROGMEM = {
  0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x03,0x95,0x58,0x5b,0x73,0xdb,0xc6,0x15,0xfe,0x2b,0x2b,0x3a,0x31,0x80,0x9a,
  0x04,0x49,0x59,0x52,0x62,0x82,0xa0,0x47,0x92,0xc5,0xf8,0x22,0x45,0xae,0xa4,0x38,0xb6,0xd3,0x4c,0x67,0x09,0x2c,0xc9,0x8d,
  0x41,0x2c,0x02,0x2c,0x28,0x31,0x14,0x67,0xfa,0x57,0xfa,0xd2,0x99,0x3c,0xf8,0xa1,0xd3,0x87,0xbe,0x57,0xff,0xa4,0xbf,0xa4,
  0xdf,0xd9,0x05,0x48,0x5d,0x92,0x76,0x3a,0x63,0x49,0x8b,0xdd,0x73,0xce,0x9e,0xcb,0x77,0x2e,0xeb,0xfe,0x56,0xac,0x22,0xbd,
  0xc8,0x04,0x9b,0xea,0x59,0x32,0xe8,0xd3,0x6f,0x96,0xf0,0x74,0x12,0x36,0x44,0xd1,0x18,0xf4,0x67,0x42,0x73,0x16,0x4d,0x79,
  0x5e,0x08,0x1d,0x36,0x4a,0x3d,0x6e,0x7d,0x5d,0xef,0xa6,0x7c,0x26,0xc2,0xc6,0x5c,0x8a,0xcb,0x4c,0xe5,0xba,0xc1,0x22,0x95,
  0x6a,0x91,0x82,0xea,0x52,0xc6,0x7a,0x1a,0xc6,0x62,0x2e,0x23,0xd1,0x32,0x1f,0x4d,0x99,0x4a,0x2d,0x79,0xd2,0x2a,0x22,0x9e,
  0x88,0xb0,0x0b,0x11,0x5a,0xea,0x44,0x0c,0x5e,0xbe,0x65,0x6f,0x0e,0x5a,0xcf,0x9e,0x7d,0xd5,0x61,0xff,0xfa,0x27,0xdb,0x8f,
  0x74,0xc9,0x13,0xf9,0x0b,0xcf,0xfb,0x6d,0x7b,0xde,0x2f,0xf4,0x02,0x7f,0x46,0x2a,0x5e,0x2c,0xc7,0x90,0xdf,0xeb,0xee,0x65,
  0x57,0xac,0x58,0x14,0x5a,0xcc,0x5a,0xa5,0x6c,0x16,0x3c,0x2d,0x5a,0x85,0xc8,0xe5,0x38,0x98,0xf1,0x2b,0x7b,0x59,0x6f,0x77,
  0xaf,0x93,0x5d,0xe1,0x3b,0x9f,0xc8,0xb4,0xf7,0x14,0x6b,0xc6,0x4b,0xad,0x82,0x8c,0xc7,0xb1,0x4c,0x27,0xbd,0x0e,0xeb,0x7e,
  0x8d,0xf3,0x11,0x8f,0x3e,0x4d,0x72,0x55,0xa6,0x71,0xef,0x51,0xb7,0xd3,0x1d,0x6d,0xef,0x04,0x91,0x4a,0x54,0xde,0x7b,0x24,
  0xf8,0x78,0x7b,0xbc,0xb7,0x9a,0x76,0xcd,0x95,0xad,0x42,0xfe,0x22,0x7a,0x5d,0x7f,0x4f,0xcc,0x56,0xd9,0x32,0x91,0xa9,0x68,
  0x4d,0x85,0x9c,0x4c,0xa1,0x8b,0xbf,0xbb,0x2a,0x66,0x3c,0x49,0x96,0x15,0xe3,0x68,0x27,0xfa,0x2a,0xee,0xae,0x1e,0xc5,0xb9,
  0xca,0x96,0xf5,0x75,0x4f,0xb7,0xa1,0x00,0xa9,0x1d,0x8c,0x54,0x1e,0x8b,0xbc,0x47,0xdf,0x31,0x2f,0xa6,0x22,0x66,0x8f,0xf6,
  0x76,0xe3,0xaf,0x46,0xbb,0xd5,0x49,0x2b,0xe7,0xb1,0x2c,0x0b,0x63,0x63,0xa0,0xc5,0x95,0x6e,0xc1,0x19,0x93,0xb4,0x17,0xc1,
  0xab,0x22,0x0f,0xa2,0x32,0x2f,0x70,0x4b,0xa6,0x24,0x7d,0xda,0x5b,0x7c,0x35,0x17,0xf9,0xf2,0xb6,0x29,0xdb,0x4f,0x77,0xa2,
  0x9d,0xa7,0x2b,0x99,0x66,0xa5,0x6e,0x8e,0x4a,0xad,0x55,0x6a,0x3d,0x27,0xd3,0x29,0xfc,0xa4,0xd7,0x6e,0xe8,0x6e,0x6f,0x54,
  0xea,0xdc,0xd3,0x00,0xfe,0xb1,0x12,0x96,0x23,0x75,0x45,0x0e,0x20,0x86,0x8a,0x04,0x3b,0x81,0xf5,0x73,0xb7,0xd3,0xf9,0xb2,
  0x72,0x73,0x4b,0xab,0xcc,0x70,0x55,0x37,0xde,0xd6,0xa8,0xb2,0xb1,0xf2,0x51,0xb7,0xb3,0x4d,0xae,0xae,0xa2,0xd3,0xdd,0x81,
  0x33,0xc0,0xc7,0x3a,0xac,0x73,0xdf,0x40,0x2b,0xaa,0x17,0xcb,0x82,0x8f,0x12,0x11,0x2f,0x55,0xc6,0x23,0xa9,0x17,0x3d,0x7f,
  0xa7,0xa6,0x8c,0xc5,0x98,0x97,0x89,0x5e,0x65,0xb9,0x9a,0xe4,0xa2,0x28,0x96,0xb7,0xf4,0xaa,0x62,0xb4,0xbd,0xbd,0x86,0x82,
  0xd1,0x91,0x42,0xbf,0x7a,0x54,0x68,0xae,0xc5,0xf2,0x72,0x2a,0xb5,0x68,0x15,0x10,0x2b,0x7a,0x59,0x0e,0xa8,0xe6,0x3c,0x0b,
  0x66,0x20,0xac,0x78,0x9f,0x22,0xe2,0x8f,0xc6,0x32,0x11,0x4b,0xe8,0x90,0x25,0x7c,0xd1,0x4b,0x55,0x2a,0x56,0xfd,0xb6,0x05,
  0x65,0x7f,0xda,0xbd,0x85,0xdf,0x7e,0x1b,0x9f,0xfd,0x6c,0xb0,0xc6,0x70,0x24,0x6f,0xfe,0x91,0x32,0xa4,0x06,0xfb,0x5e,0xb6,
  0x86,0xd2,0xef,0xb7,0x33,0x9c,0x33,0x19,0x87,0x0d,0x99,0x8e,0x55,0x63,0x70,0xa8,0xd2,0x02,0xda,0xf3,0x34,0x56,0x4c,0x24,
  0x4c,0x8b,0x28,0xe1,0xb1,0xfa,0xf7,0x5f,0xfe,0x66,0x28,0x63,0x39,0x37,0xb4,0x14,0xe5,0x06,0xcb,0x15,0xd2,0xa6,0x61,0x3d,
  0xd2,0x60,0x9a,0x8f,0x64,0x1a,0x8b,0xab,0xb0,0xd1,0x69,0x30,0x9e,0x4b,0xde,0x4a,0xf8,0x48,0x24,0x61,0xe3,0x28,0x11,0x13,
  0x99,0xb3,0xb1,0xcc,0x67,0x97,0x3c,0x17,0x48,0xb3,0x91,0x11,0x42,0xa9,0xda,0x18,0xec,0xe7,0x39,0x2f,0x74,0x7e,0xf3,0x57,
  0x56,0xa6,0x60,0x8b,0xa6,0x72,0xae,0x98,0x0f,0x51,0x8c,0xff,0x5c,0xde,0x7c,0xee,0xb7,0x47,0x64,0xc0,0x29,0xd3,0x2a,0x02,
  0x4d,0xc6,0x73,0x0e,0xbd,0x48,0x60,0xa2,0x8c,0x4a,0x6d,0xe8,0x34,0xe8,0x1b,0x5c,0x18,0xa9,0xe4,0x1a,0x28,0x83,0xe2,0x51,
  0xaf,0x79,0x14,0x89,0x0c,0x15,0x80,0x84,0x36,0x48,0x58,0xdf,0x28,0xc6,0xc6,0x2a,0x0f,0x1b,0x9f,0xc4,0xc2,0x18,0xad,0xa1,
  0x85,0xb8,0xf9,0x3b,0x67,0xb1,0x60,0x11,0x22,0xc3,0xfb,0x6d,0x43,0x75,0x5b,0x34,0xd1,0x56,0x92,0x33,0x5e,0x14,0x97,0x80,
  0x5e,0x83,0x21,0x04,0x91,0x98,0xaa,0x04,0x28,0x0c,0x1b,0x95,0xbb,0x1a,0x26,0xb7,0x23,0x35,0xcb,0x12,0xa1,0x41,0xad,0xc6,
  0x63,0x5c,0x6c,0xb2,0xf2,0xce,0x5d,0x28,0x4e,0x63,0x39,0x29,0x81,0x6f,0xde,0xab,0x5d,0x8d,0x40,0x1a,0x3a,0x63,0x9c,0xf5,
  0xad,0xb9,0xbc,0x10,0x29,0x6e,0xab,0x71,0xb7,0x89,0x68,0xbe,0x61,0xb4,0xd4,0x77,0xb8,0xc4,0x95,0xd4,0xb7,0xb8,0xde,0xa9,
  0x04,0xa9,0xc9,0x78,0xf2,0x90,0xa9,0x86,0xab,0x61,0xab,0x3f,0x1a,0x0c,0x05,0x2c,0x6c,0x00,0xbb,0x0d,0x36,0xe7,0x49,0x29,
  0x1e,0x04,0xf7,0xad,0xa5,0x04,0x72,0xa0,0x70,0xc5,0x55,0x23,0xca,0x00,0xba,0x86,0x09,0x7d,0x94,0x45,0xcd,0x2c,0xe7,0xe4,
  0x44,0x95,0x00,0xeb,0x8d,0xc1,0x51,0x91,0x89,0xdc,0x20,0x0e,0x0e,0x81,0xc6,0x80,0xa8,0x05,0xa6,0xf5,0xc4,0x09,0xa7,0x12,
  0x7e,0xf3,0x2b,0x3a,0x00,0x34,0x97,0x33,0x14,0x1e,0x8b,0x63,0x9f,0x5d,0xc0,0x93,0x88,0xef,0xda,0x15,0x73,0x63,0x1f,0x70,
  0x72,0x00,0x5d,0xb5,0x52,0x7a,0xca,0x16,0xac,0x40,0x44,0x05,0x50,0x46,0xfb,0x02,0x6a,0xb0,0x1c,0x45,0xae,0x86,0x7f,0xe5,
  0xec,0x22,0xca,0x65,0xa6,0x07,0xae,0xeb,0x85,0x83,0xa5,0x53,0x82,0x05,0x98,0x94,0x91,0x76,0x50,0x21,0xd2,0x42,0xb3,0x2f,
  0x42,0x18,0x34,0x40,0x57,0x2a,0xe9,0x7a,0x7f,0x22,0x34,0x50,0x4d,0xcb,0x83,0xc5,0xab,0xd8,0x95,0xb1,0xd7,0x7c,0x11,0x7e,
  0xe1,0x3a,0x94,0x17,0x8e,0xd7,0x7c,0x45,0x6b,0x02,0x1f,0xd6,0x6f,0x68,0x0d,0xe4,0x60,0xb9,0x4f,0x4b,0x8a,0x23,0xd6,0x47,
  0xb4,0xa6,0xe8,0x60,0xfd,0x0d,0xad,0x6b,0xe7,0xe1,0xfb,0xa5,0xa1,0x23,0xe7,0x39,0x5e,0x00,0x04,0xb1,0x61,0x98,0x96,0x49,
  0xd2,0x3c,0x0e,0x3b,0xcd,0xb3,0x70,0xcc,0x93,0x42,0x34,0x0f,0xaa,0xbf,0x17,0xcd,0xc3,0x4a,0xc5,0x93,0x50,0x87,0x83,0x97,
  0x3e,0x95,0xe8,0xc3,0xaa,0xe9,0xe9,0x60,0x5c,0xa6,0x91,0x96,0x00,0xc3,0x77,0xae,0xb7,0xdc,0xf7,0x6b,0x20,0x84,0x07,0xd7,
  0xd7,0x5b,0x67,0xf8,0x19,0xe2,0xe7,0x8d,0x6f,0x42,0x1b,0x1c,0x3d,0x3c,0xae,0x8f,0xde,0xdc,0x3a,0x0a,0x5e,0xf8,0x68,0xbb,
  0xfb,0x1a,0xfe,0x01,0x76,0x84,0xeb,0x98,0x88,0xd6,0xe7,0x4e,0xf3,0x1c,0x07,0xe9,0xc4,0x3d,0xf0,0xbc,0xd5,0xfa,0xf6,0x77,
  0xee,0xd8,0x5b,0xe6,0x42,0x97,0x79,0xca,0xb6,0xda,0x7f,0xa2,0x4c,0xfc,0xa2,0x2d,0xa1,0x6b,0xa1,0xdd,0xb1,0x4f,0x85,0xc0,
  0xbb,0xbe,0x6e,0xbb,0xcf,0x7b,0x33,0x91,0x4f,0x44,0x7c,0x3d,0x42,0xe8,0x12,0xc5,0x91,0x53,0xd7,0xc8,0x78,0x74,0x5d,0x08,
  0x29,0xcc,0xee,0x9f,0x79,0x96,0x75,0xbc,0x7b,0xbc,0xcf,0x9d,0xef,0x0a,0x0a,0x6e,0x62,0x0b,0x07,0x52,0x98,0x67,0x89,0x8c,
  0x2c,0x4a,0x9a,0x2c,0x55,0x6c,0x23,0x90,0xa5,0x92,0xd9,0x5b,0x7c,0xa7,0x37,0xf6,0xa9,0x8f,0xf6,0xb7,0x77,0x9e,0x3b,0xfb,
  0x55,0xe9,0x99,0xf3,0xe8,0xe6,0xb3,0x62,0x8a,0xc9,0xb4,0x4a,0x60,0xb5,0x26,0x1c,0x1c,0x3f,0x77,0x8e,0x92,0x75,0x95,0x82,
  0xdc,0x08,0x59,0x80,0x63,0x0a,0xcf,0xc6,0xda,0x73,0x37,0x91,0x85,0xf6,0x96,0x72,0x0c,0x2f,0x58,0xab,0x03,0x1b,0xc2,0x00,
  0x81,0x25,0x8d,0x1d,0xef,0x4e,0x9c,0x9c,0xff,0x51,0x04,0x9d,0xe0,0x1b,0x1b,0x88,0xb0,0x13,0x40,0xe8,0xd6,0x99,0xb7,0x3c,
  0x71,0x1d,0x93,0x36,0x60,0x49,0x08,0xd1,0x45,0x56,0x1a,0x6c,0xc7,0x9b,0xa2,0xed,0x03,0x3d,0x08,0x7b,0x60,0x35,0x58,0x81,
  0x91,0xd4,0xf2,0x13,0x91,0x4e,0xf4,0x74,0x2b,0x0c,0xbb,0x56,0x0a,0x8a,0xe9,0xcd,0x67,0xba,0xb7,0x40,0x2f,0xac,0x2f,0xbf,
  0xc7,0x6b,0x11,0x36,0x0e,0x49,0xc0,0x0f,0x9d,0x1f,0x9b,0x48,0x27,0x14,0x4f,0x8a,0x2a,0xe9,0x63,0xbe,0x48,0x98,0x5d,0xdc,
  0x66,0x1c,0x86,0xe3,0xdf,0xb1,0xd9,0x86,0xee,0x89,0x43,0x23,0x96,0xf3,0xc4,0xfa,0xd7,0xd7,0xea,0x58,0xd1,0x30,0x56,0x61,
  0xc8,0xc3,0xf1,0x68,0x81,0x48,0x3b,0x01,0x34,0x3d,0xc6,0xe5,0xca,0x67,0x47,0x94,0xab,0x23,0xa8,0x9c,0x98,0x02,0xba,0x2e,
  0xa6,0x0b,0x96,0x95,0x09,0xc1,0x60,0x53,0x1a,0x2b,0x2b,0x56,0xbc,0x58,0xa4,0x11,0x5b,0xc7,0xe7,0xad,0x7b,0x37,0x36,0x87,
  0x61,0x2a,0x2e,0xd9,0x3e,0x46,0x09,0xa3,0x1d,0xea,0x55,0x22,0x72,0xd8,0xa0,0xf3,0xc5,0xd2,0x5a,0xfe,0x3a,0xe4,0x97,0x5c,
  0xc2,0x03,0x42,0x47,0x53,0xd7,0x69,0xdb,0x62,0xe6,0x34,0x97,0xc0,0xd8,0x54,0xf4,0x9c,0x54,0xb5,0xa0,0x5b,0x2e,0x9c,0x66,
  0x81,0xd9,0x88,0x27,0xbd,0x43,0xdf,0x2e,0x56,0xc6,0x41,0x5b,0xaf,0x7d,0xf5,0xc9,0xd3,0xd3,0x5c,0x5d,0xb2,0x23,0xf2,0x11,
  0x84,0x5b,0xc1,0x45,0x25,0xf8,0xb5,0xff,0x53,0xa1,0x52,0xd7,0x50,0xaf,0xf5,0x3a,0x0e,0xbf,0x2d,0x67,0x23,0xa8,0x52,0xf8,
  0x28,0xc3,0x07,0xe4,0x08,0x2f,0x38,0xab,0x36,0x7d,0x59,0x0c,0x69,0x80,0x15,0xee,0xb1,0xf7,0xf8,0xf1,0xf1,0xa0,0xf3,0xf8,
  0x31,0xe8,0x4c,0xd5,0x08,0xc3,0xd0,0x91,0x31,0xaa,0xcf,0xf5,0xf5,0xad,0x1d,0x13,0x1c,0x67,0x7d,0xb1,0xc0,0xdf,0xb8,0x08,
  0x4f,0xb8,0x9e,0x92,0x74,0xb7,0xd3,0x5c,0x5f,0x96,0x8b,0x19,0x87,0xe8,0x74,0x72,0x6e,0x89,0x90,0x9b,0x1d,0x8f,0xc2,0x48,
  0x23,0xc2,0x7d,0xe8,0xbe,0x13,0x79,0x61,0x86,0x0a,0xe7,0x49,0xe1,0xcf,0xe9,0x43,0xa5,0x75,0x54,0x8f,0x7f,0x37,0xa0,0xd4,
  0x92,0x32,0x95,0x4a,0x94,0x8c,0x82,0x68,0x23,0x09,0xf5,0x04,0x13,0x24,0xc6,0xa8,0x34,0x4e,0x14,0xdc,0x54,0x69,0xd9,0xde,
  0xeb,0x80,0xb3,0xe7,0x3c,0xa9,0xc4,0x54,0xdb,0x5f,0x62,0xdb,0xc7,0xe4,0x78,0xae,0x51,0x23,0xdc,0xed,0xa6,0xd3,0x71,0x8c,
  0x03,0x87,0x8f,0x1f,0xbf,0x73,0x87,0x9e,0xb7,0xac,0x92,0xce,0xa0,0xfc,0x61,0xd6,0x7a,0x94,0x12,0x5b,0x43,0xef,0x04,0x26,
  0x1b,0xe7,0x5c,0x5f,0x33,0xf7,0xec,0x79,0x9d,0x11,0x34,0x21,0x54,0x23,0x0b,0xfb,0xb9,0x34,0x3f,0xf9,0xcd,0xaf,0xe8,0x8f,
  0x70,0x1f,0x4f,0x08,0x5c,0x3d,0x12,0x5b,0x25,0x1b,0x75,0x16,0x60,0x0f,0x0d,0x02,0x0a,0xa9,0x9e,0xf1,0x86,0x71,0xbd,0xe7,
  0x05,0xab,0x88,0x13,0x6c,0x84,0x41,0xdd,0xd6,0x81,0xb7,0xac,0x8a,0x38,0x29,0x76,0x8e,0x1c,0x5f,0x67,0xaf,0xcf,0xce,0xf0,
  0x3e,0xa9,0x4a,0x99,0xe9,0x50,0xa8,0x43,0xb9,0x40,0x83,0x93,0x73,0x6c,0x9e,0x5e,0xec,0x93,0xd6,0xab,0xb1,0x04,0xb2,0x92,
  0x45,0x2d,0x8d,0xd2,0xed,0x22,0x44,0x4d,0xbe,0x40,0x6f,0x54,0xa5,0x76,0xdf,0x36,0x9f,0x76,0x3a,0x1d,0x10,0x6e,0x4a,0xd2,
  0x1f,0x01,0xf9,0x28,0x11,0x3c,0xaf,0x69,0x2e,0x8c,0xa7,0x0e,0xbd,0x43,0x9f,0x13,0xee,0xdd,0x5b,0xc5,0xfa,0x7b,0x37,0x43,
  0x04,0x6c,0x8a,0xdc,0xed,0x03,0x35,0x30,0x71,0x42,0x24,0x84,0xaa,0x76,0x99,0xc5,0xd4,0xa8,0x80,0x3e,0x6a,0x26,0xc6,0xef,
  0x35,0xd9,0x41,0xa8,0x73,0x34,0x0f,0xdc,0x6d,0x4a,0xc2,0xc9,0x03,0x26,0xf8,0x3a,0x9d,0x4b,0xd3,0xf9,0x6b,0x57,0x63,0xd2,
  0x84,0x5f,0x69,0x4e,0x91,0x82,0xf6,0xf9,0xed,0x19,0x74,0x8d,0xde,0xf7,0x26,0x63,0xdf,0x9f,0x1c,0xbf,0xd4,0x3a,0x3b,0x13,
  0x3f,0x93,0xf7,0x70,0xc3,0x7b,0x5f,0x65,0x22,0x75,0x9d,0xb7,0xa7,0xe7,0x17,0x4e,0xd3,0x58,0x81,0x3d,0x6d,0x4d,0x0e,0x9f,
  0xed,0xc1,0x2b,0x1d,0x6c,0xc0,0x55,0x15,0xcf,0x4b,0x41,0x2d,0xc1,0x75,0xde,0xb7,0xe0,0xdb,0xd6,0x1b,0x74,0xe9,0x66,0x6d,
  0xaa,0x69,0xbc,0x1f,0x2c,0x80,0x7e,0xc3,0x5e,0x6f,0xf9,0xdb,0x72,0x86,0x95,0x1d,0xad,0x73,0xd4,0xb3,0x75,0x33,0x1c,0x9a,
  0xf2,0x06,0x28,0x7c,0x30,0x8a,0x0f,0x55,0x3e,0x7b,0xc1,0x35,0x87,0xca,0x1f,0x7c,0x34,0x34,0x58,0x4a,0xf3,0x82,0xe5,0x74,
  0x9a,0xc3,0xe6,0xd0,0xf6,0x35,0xe8,0x5a,0x66,0xd4,0xb6,0x7c,0x95,0xd6,0xf3,0x41,0x28,0x30,0xa1,0x50,0xd1,0xad,0x0a,0xf9,
  0x21,0x9a,0x54,0xa9,0xa9,0xf7,0x7a,0xcb,0xba,0x4b,0xd8,0xa4,0x96,0xa9,0xfb,0x6c,0xb7,0x69,0xd6,0xe6,0xd1,0x43,0x2c,0xd4,
  0x02,0xe3,0x36,0x15,0x5a,0x60,0xf8,0x0f,0xcf,0x76,0x3d,0xcf,0x64,0x47,0x15,0x05,0x82,0x6d,0x25,0xe3,0x89,0xf3,0x25,0x41,
  0xed,0xb6,0x06,0xf4,0x27,0x34,0x03,0x52,0x7d,0xcf,0xb3,0x5d,0xe2,0x7e,0x87,0x62,0x1b,0xd7,0x0f,0x86,0x2a,0xcb,0x4c,0xb0,
  0x56,0x55,0xdf,0xf8,0x48,0x93,0xc9,0xb2,0x9a,0x58,0x82,0x0d,0xf8,0xb5,0xc5,0xc5,0x6f,0x62,0x97,0x62,0x79,0xeb,0x46,0x0a,
  0xc6,0xa9,0x29,0xc8,0xa7,0xe1,0xeb,0xf3,0xd3,0x6f,0x91,0xf6,0x78,0xfc,0xbb,0xef,0x7d,0xca,0x1d,0x5c,0x22,0x2e,0x50,0x93,
  0xbc,0x4d,0xa2,0x51,0x6a,0xbf,0xf7,0x6d,0x99,0x46,0xd8,0xb6,0x3b,0x28,0x8f,0xa7,0xf8,0x87,0x3a,0x8c,0x4f,0x82,0xe5,0xc6,
  0x5b,0x0f,0x50,0x89,0x71,0x17,0x8f,0xcf,0x13,0xf7,0xd4,0x9f,0xc1,0xe1,0x7c,0x22,0xae,0xaf,0x37,0xcd,0xc7,0xf6,0x5c,0x2a,
  0x08,0xf3,0x52,0x24,0x73,0xbe,0x19,0x35,0x29,0x37,0x05,0x2c,0x63,0x1f,0x5d,0x73,0x55,0x55,0x53,0x9c,0xb3,0x75,0x73,0xc6,
  0x33,0xdd,0x8c,0xba,0x31,0x67,0xee,0xcb,0x8b,0x8b,0xb7,0x70,0x77,0xad,0xe4,0x13,0xd4,0x55,0xa7,0x32,0xdb,0x76,0x57,0xb2,
  0xfb,0x23,0x0a,0x84,0x60,0x60,0x89,0x51,0x62,0xab,0x86,0x57,0x0d,0xc8,0x8c,0x02,0x9f,0xab,0x91,0x2d,0x16,0x75,0x15,0xa2,
  0x51,0xb9,0xa0,0x01,0x28,0x17,0x99,0xd0,0xd2,0x74,0x40,0x12,0x59,0xe7,0x40,0x25,0xf4,0x42,0x8a,0x59,0x06,0xea,0x09,0x60,
  0x80,0x41,0xe1,0xff,0x91,0x45,0xf3,0xab,0xfb,0xc1,0x5b,0xbd,0x80,0xd4,0x08,0x43,0xd6,0x27,0x1b,0x20,0x5b,0x8b,0x5e,0xf9,
  0x66,0x0b,0xd5,0x24,0xa0,0x73,0x8c,0xbd,0xb1,0xba,0x4c,0x37,0x90,0xc5,0x06,0x79,0xfa,0x88,0x9e,0xd2,0xe8,0x4e,0xeb,0x0d,
  0x86,0x7c,0x12,0x3e,0x9e,0xbb,0x73,0xf4,0x94,0x17,0xf6,0x01,0x6d,0x9b,0xe1,0x1d,0xa1,0xab,0xe0,0x15,0xdd,0x3a,0xe5,0xe9,
  0x44,0x98,0x6b,0xcf,0xdd,0x57,0x3e,0xcd,0xd9,0xe8,0x8b,0x6f,0x70,0x62,0x9e,0x6a,0xe1,0x77,0xc1,0xfe,0x1d,0xdd,0xbe,0x77,
  0x37,0x59,0x8b,0x21,0xf7,0xfe,0x91,0x1d,0xc2,0x03,0xbc,0x06,0x5d,0x0b,0x57,0xa3,0x04,0x53,0x63,0xf6,0x03,0xe6,0x79,0xb8,
  0x08,0x4d,0xcd,0x69,0xda,0xd1,0xfe,0x47,0x6f,0xfd,0x04,0xe0,0x71,0x7c,0x44,0x84,0x04,0x0c,0x81,0x90,0xb9,0x86,0xad,0x09,
  0x53,0x1f,0x1a,0xe2,0x19,0x6f,0xd4,0xc2,0xcc,0xcd,0x2f,0x60,0x14,0xde,0x90,0xc4,0x4d,0xa2,0x5c,0xc7,0x5c,0xb3,0x21,0x44,
  0xad,0x9e,0x8b,0x07,0x94,0xe8,0xcc,0xa0,0xbb,0x47,0xac,0x32,0xe3,0xe0,0xff,0x42,0x77,0x0e,0xd7,0xc3,0x7e,0x8e,0xe7,0x52,
  0x5a,0x8c,0x31,0x3b,0x58,0x9f,0xad,0x02,0x4c,0x42,0xc1,0xca,0xc3,0x2f,0x3c,0x84,0xec,0x0b,0xa8,0xdf,0x36,0xff,0xe5,0xf6,
  0x1f,0x68,0xdf,0x6d,0x62,0x82,0x13,0x00,0x00,
};

// Servidor HTTP del firmware V2.0.0. OTA_WEB_PAGE_GZ debe declararse antes de este modulo.
// Todas estas funciones corren en loop(), el mismo contexto de WebServer.
// El HTML es publico; solamente escribir firmware o salir requiere la clave.
constexpr uint32_t WEB_UPLOAD_IDLE_MS = 30000;
constexpr size_t WEB_IMAGE_PREFIX_BYTES = 36;
constexpr uint32_t WEB_MIN_IMAGE_BYTES = 288; // cabecera24 + segmento8 + app_desc256
bool webRequestSeen = false, webFinalHandled = false;
bool webUploadStarted = false, webUploadFailed = false, webUploadComplete = false;
bool webHeaderChecked = false;
uint8_t webFileCount = 0;
uint32_t webUploadBytes = 0, webExpectedBytes = 0, webLastChunkMs = 0;
size_t webPrefixLength = 0;
uint8_t webPrefix[WEB_IMAGE_PREFIX_BYTES] = {};
int webErrorStatus = 400;
char webLastError[160] = "";
const char *webState = "idle";

uint32_t webMaxImageBytes() {
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  return target && otaPartitionsValid() ? static_cast<uint32_t>(target->size) : 0;
}

// Solo numeros decimales positivos; sin signos, espacios ni truncamiento.
bool webReadSize(const String &value, uint32_t &result) {
  if (!value.length() || value.length() > 10) return false;
  uint32_t number = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (number > (UINT32_MAX - digit) / 10UL) return false;
    number = number * 10UL + digit;
  }
  if (!number) return false;
  result = number;
  return true;
}

String webJSONText(const char *text) {
  String escaped;
  escaped.reserve(192);
  if (!text) return escaped;
  for (size_t i = 0; text[i] && i < 159; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') escaped += '\\';
    if (c >= 0x20) escaped += static_cast<char>(c);
    else escaped += ' ';
  }
  return escaped;
}

void webResponseHeaders() {
  otaWeb.sendHeader("Cache-Control", "no-store, max-age=0");
  otaWeb.sendHeader("Connection", "close");
  otaWeb.sendHeader("X-Content-Type-Options", "nosniff");
}

void webReply(int status, bool ok, const char *message) {
  webResponseHeaders();
  String body = ok ? "{\"ok\":true,\"message\":\"" : "{\"ok\":false,\"error\":\"";
  body += webJSONText(message);
  body += "\"}";
  otaWeb.send(status, "application/json; charset=utf-8", body);
}

bool authorizedWeb() {
  return otaWeb.header("X-OTA-Key") == OTA_UPLOAD_PASSWORD;
}

void webFail(int httpStatus, const char *message, int errorCode) {
  // Conservar el primer motivo: un ABORT posterior no debe tapar la causa.
  if (!webUploadFailed) {
    webErrorStatus = httpStatus;
    snprintf(webLastError, sizeof(webLastError), "%s", message ? message : "Error de carga");
    lastOtaError = errorCode;
    logf("[WEB] %s\n", webLastError);
  }
  webUploadFailed = true;
  webUploadComplete = false;
  webState = "error";
  if (Update.isRunning()) Update.abort();
  otaBusy = false;
}

void resetWebUploadState() {
  // El error visible se conserva hasta que comience otra carga valida.
  webRequestSeen = false;
  webFinalHandled = false;
  webUploadStarted = false;
  webUploadFailed = false;
  webUploadComplete = false;
  webHeaderChecked = false;
  webFileCount = 0;
  webUploadBytes = 0;
  webExpectedBytes = 0;
  webPrefixLength = 0;
  webErrorStatus = 400;
}

bool webAcceptFilename(const String &filename) {
  String name = filename;
  name.toLowerCase();
  return name.endsWith(".bin") && name.indexOf("merged") < 0 &&
      name.indexOf("bootloader") < 0 && name.indexOf("partition") < 0 &&
      name.indexOf("boot_app0") < 0 && name.indexOf("bootapp0") < 0;
}

bool webImagePrefixValid() {
  // ESP-IDF: imagen magic E9; chip_id little endian en offset12, C3=0x0005.
  // El descriptor de aplicacion comienza en offset32 (24+8), magic ABCD5432.
  // Excluye tambien un bootloader renombrado o una imagen merged.
  return webPrefixLength == WEB_IMAGE_PREFIX_BYTES && webPrefix[0] == 0xE9 &&
      webPrefix[1] > 0 && webPrefix[1] <= 16 &&
      webPrefix[12] == 0x05 && webPrefix[13] == 0x00 &&
      webPrefix[32] == 0x32 && webPrefix[33] == 0x54 &&
      webPrefix[34] == 0xCD && webPrefix[35] == 0xAB;
}

bool webCheckUploadTime() {
  if (uint32_t(millis() - otaTransferStartMs) >= OTA_TRANSFER_LIMIT_MS) {
    webFail(408, "La transferencia excedio el tiempo permitido.", 11);
    return false;
  }
  if (uint32_t(millis() - webLastChunkMs) >= WEB_UPLOAD_IDLE_MS) {
    webFail(408, "La transferencia se interrumpio: no llegaron datos.", 12);
    return false;
  }
  return true;
}

void serviceWebUploadTimeout() {
  if (otaBusy && !webUploadFailed) webCheckUploadTime();
}

void webUploadEvent() {
  HTTPUpload &upload = otaWeb.upload();
  feedWatchdog();
  if (upload.status == UPLOAD_FILE_START) {
    webRequestSeen = true;
    if (webFileCount < 255) ++webFileCount;
    if (webFileCount != 1) {
      webFail(400, "Selecciona un solo archivo de firmware por carga.", 13);
      return;
    }
    if (otaRebootPending || otaClosing || !otaMode) {
      webFail(409, "El teclado esta saliendo del mantenimiento.", 14);
      return;
    }
    if (!authorizedWeb()) {
      webFail(403, "Clave de actualizacion incorrecta.", 15);
      return;
    }
    if (upload.name != "firmware" || !webAcceptFilename(upload.filename)) {
      webFail(400, "Selecciona un .bin de aplicacion para ESP32-C3.", 16);
      return;
    }
    if (!webReadSize(otaWeb.header("X-Firmware-Size"), webExpectedBytes) ||
        webExpectedBytes < WEB_MIN_IMAGE_BYTES) {
      webFail(400, "Falta el tamano del firmware o el archivo esta incompleto.", 17);
      return;
    }
    const uint32_t limit = webMaxImageBytes();
    if (!limit) {
      webFail(409, "No hay una particion OTA compatible disponible.", 18);
      return;
    }
    if (webExpectedBytes > limit) {
      webFail(413, "El firmware no cabe en la particion OTA del teclado.", 19);
      return;
    }
    webUploadStarted = true;
    webLastError[0] = '\0';
    lastOtaError = -1;
    otaProgress = 0;
    otaBusy = true;
    otaTransferStartMs = millis();
    webLastChunkMs = otaTransferStartMs;
    webState = "uploading";
    setPowerForOTA(true);
    logf("[WEB] Carga autorizada: %lu bytes; slot=%lu.\n",
         (unsigned long)webExpectedBytes, (unsigned long)limit);
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    if (webRequestSeen) webFail(400, "La carga se cancelo antes de terminar.", 20);
    return;
  }
  if (webUploadFailed) return;
  if (!webUploadStarted || webFileCount != 1) {
    webFail(400, "No se recibio un inicio de carga valido.", 21);
    return;
  }
  if (!webCheckUploadTime()) return;

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (webUploadComplete || upload.currentSize > webExpectedBytes - webUploadBytes) {
      webFail(400, "Los datos recibidos no coinciden con el tamano anunciado.", 22);
      return;
    }
    if (!upload.currentSize) return;
    webLastChunkMs = millis();
    size_t offset = 0;
    if (!webHeaderChecked) {
      const size_t needed = WEB_IMAGE_PREFIX_BYTES - webPrefixLength;
      const size_t take = upload.currentSize < needed ? upload.currentSize : needed;
      memcpy(webPrefix + webPrefixLength, upload.buf, take);
      webPrefixLength += take;
      offset += take;
      if (webPrefixLength == WEB_IMAGE_PREFIX_BYTES) {
        if (!webImagePrefixValid()) {
          webFail(400, "Este archivo no es una aplicacion ESP32-C3 valida.", 23);
          return;
        }
        if (!Update.begin(webExpectedBytes, U_FLASH)) {
          webFail(500, Update.errorString(), 24);
          return;
        }
        webHeaderChecked = true;
        if (Update.write(webPrefix, WEB_IMAGE_PREFIX_BYTES) != WEB_IMAGE_PREFIX_BYTES) {
          webFail(500, Update.errorString(), 25);
          return;
        }
      }
    }
    if (webHeaderChecked && offset < upload.currentSize &&
        Update.write(upload.buf + offset, upload.currentSize - offset) != upload.currentSize - offset) {
      webFail(500, Update.errorString(), 26);
      return;
    }
    webUploadBytes += static_cast<uint32_t>(upload.currentSize);
    otaProgress = static_cast<unsigned int>(uint64_t(webUploadBytes) * 99ULL / webExpectedBytes);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (webUploadComplete || !webHeaderChecked || webUploadBytes != webExpectedBytes ||
        upload.totalSize != webExpectedBytes || !Update.isRunning()) {
      webFail(400, "El archivo llego incompleto; el firmware anterior sigue activo.", 27);
      return;
    }
    webUploadComplete = true;
    webState = "validating";
    // NO confirmar aqui: aun faltan el limite final multipart y el handler POST.
    // Un segundo archivo o una desconexion todavia deben poder abortar.
  }
}

void webFinishUpload() {
  webFinalHandled = true;
  feedWatchdog();
  if (!authorizedWeb()) {
    webFail(403, "Clave de actualizacion incorrecta.", 15);
  } else if (otaRebootPending || otaClosing || !otaMode) {
    webFail(409, "El teclado esta saliendo del mantenimiento.", 14);
  } else if (!webUploadFailed && (!webRequestSeen || webFileCount != 1 ||
             !webUploadStarted || !webUploadComplete || !webHeaderChecked ||
             webUploadBytes != webExpectedBytes || !Update.isRunning())) {
    webFail(400, "No se recibio un archivo completo para actualizar.", 28);
  }
  if (!webUploadFailed && webCheckUploadTime()) {
    // El tamano es exacto: end(false) no acepta truncar una imagen incompleta.
    if (!Update.end(false)) webFail(400, Update.errorString(), 29);
  }
  if (webUploadFailed) {
    webReply(webErrorStatus, false, webLastError);
    otaBusy = false;
    return;
  }
  otaBusy = false;
  otaProgress = 100;
  webState = "rebooting";
  webLastError[0] = '\0';
  lastOtaError = -1;
  webReply(200, true, "Firmware validado. El teclado reiniciara en unos segundos.");
  scheduleOtaReboot("[WEB] Imagen validada; reinicio al firmware nuevo.");
}

void serviceWebOTAClient() {
  // WebServer completa parseo, callbacks de archivo y handler final dentro de
  // una misma invocacion sincrona. No compartir flags entre peticiones HTTP.
  if (otaBusy || Update.isRunning()) {
    webFail(400, "La peticion anterior no termino correctamente.", 30);
  }
  resetWebUploadState();
  otaWeb.handleClient();
  if (webRequestSeen && !webFinalHandled && !otaRebootPending) {
    webFail(400, "La conexion se cerro antes de confirmar la carga completa.", 31);
  }
}

void webShowPage() {
  webResponseHeaders();
  otaWeb.sendHeader("Content-Encoding", "gzip");
  otaWeb.send_P(200, "text/html; charset=utf-8",
      reinterpret_cast<const char *>(OTA_WEB_PAGE_GZ), sizeof(OTA_WEB_PAGE_GZ));
}

void webShowStatus() {
  const uint32_t elapsed = uint32_t(millis() - otaStartMs);
  const uint32_t remaining = elapsed < OTA_WINDOW_MS ? (OTA_WINDOW_MS - elapsed + 999) / 1000 : 0;
  char prefix[224];
  snprintf(prefix, sizeof(prefix),
      "{\"version\":\"%s\",\"maxBytes\":%lu,\"remainingSeconds\":%lu,\"state\":\"%s\",\"error\":\"",
      FW_VERSION, (unsigned long)webMaxImageBytes(), (unsigned long)remaining,
      otaRebootPending ? "rebooting" : webState);
  String body = prefix;
  body += webJSONText(webLastError);
  body += "\"}";
  webResponseHeaders();
  otaWeb.send(200, "application/json; charset=utf-8", body);
}

void configureWebOTA() {
  static const char *headers[] = {"X-OTA-Key", "X-Firmware-Size"};
  otaWeb.collectHeaders(headers, 2);
  resetWebUploadState();
  webLastError[0] = '\0';
  webState = "idle";
  otaWeb.on("/", HTTP_GET, webShowPage);
  otaWeb.on("/", HTTP_HEAD, []() {
    webResponseHeaders();
    otaWeb.sendHeader("Content-Encoding", "gzip");
    otaWeb.setContentLength(sizeof(OTA_WEB_PAGE_GZ));
    otaWeb.send(200, "text/html; charset=utf-8", "");
  });
  otaWeb.on("/status", HTTP_GET, webShowStatus);
  otaWeb.on("/update", HTTP_POST, webFinishUpload, webUploadEvent);
  otaWeb.on("/exit", HTTP_POST, []() {
    if (!authorizedWeb()) {
      webReply(403, false, "Clave de actualizacion incorrecta.");
      return;
    }
    if (otaBusy || Update.isRunning() || otaRebootPending) {
      webReply(409, false, "Espera a que termine la operacion en curso.");
      return;
    }
    webReply(200, true, "El teclado volvera a Bluetooth en unos segundos.");
    webState = "rebooting";
    scheduleOtaReboot("[WEB] Salida solicitada desde la pagina.");
  });
  otaWeb.on("/favicon.ico", HTTP_GET, []() {
    webResponseHeaders();
    otaWeb.send(204, "image/x-icon", "");
  });
  // Las comprobaciones HTTP de portales locales llegan a la misma pagina.
  otaWeb.on("/hotspot-detect.html", HTTP_GET, webShowPage);
  otaWeb.on("/generate_204", HTTP_GET, webShowPage);
  otaWeb.onNotFound([]() {
    if (otaWeb.method() == HTTP_GET || otaWeb.method() == HTTP_HEAD) {
      webResponseHeaders();
      otaWeb.sendHeader("Location", "http://192.168.4.1/");
      otaWeb.send(302, "text/plain; charset=utf-8", "Abrir http://192.168.4.1/");
    } else {
      webReply(404, false, "Ruta no encontrada.");
    }
  });
}


void serviceOTA() {
  if (otaRequested && uint32_t(millis() - otaRequestMs) >= OTA_RELEASE_WAIT_MS) {
    otaRequested = false;
    resetOtaChord(true);
    logLine("[OTA] Entrada cancelada: teclas/bus ocupados. Soltar y reintentar.");
  }
  if (!otaMode) {
    if (otaRequested) beginOTA();
    return;
  }
  feedWatchdog();
  if (otaRebootPending) {
    if (uint32_t(millis() - otaRestartMs) >= OTA_REBOOT_GRACE_MS) ESP.restart();
    return;
  }
  if (otaClosing) {
    if (uint32_t(millis() - wifiStopAttemptMs) >= 1000) finishOtaExit();
    return;
  }
  serviceWebUploadTimeout();
  if (!otaBusy && uint32_t(millis() - otaStartMs) >= OTA_WINDOW_MS) {
    stopOTA();
    return;
  }
  serviceWebOTAClient(); // Parser HTTP sincrono; tarea de alimentacion independiente.
  feedWatchdog();
  serviceWebUploadTimeout();
  if (!otaBusy && !otaRebootPending &&
      uint32_t(millis() - otaStartMs) >= OTA_WINDOW_MS) stopOTA();
}


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



// ================= DIAGNÓSTICO =================
void printStats() {
  const PowerState power = powerSnapshot();
  const uint32_t avg = queueSamples ? static_cast<uint32_t>(sumQueueWaitMs / queueSamples) : 0;
  logf("\n=== HP KB9970 V%s ===\n", FW_VERSION);
  logf("[BLE] CONNECTED=%d PAIRED=%d READY=%d INTERVAL_REQUEST=%u ms\n", keyboard.isConnected(), keyboard.isPaired(), readyPrev, BLE_INTERVAL_REQUEST_MS);
  logf("[PS2] BYTES=%lu MAKE=%lu BREAK=%lu PAUSE=%lu PARITY=%lu STOP=%lu CUT=%lu DROP=%lu RESYNC=%lu\n",
      (unsigned long)bytesOK, (unsigned long)makes, (unsigned long)breaks, (unsigned long)pauses,
      (unsigned long)badParity, (unsigned long)badStop, (unsigned long)cutFrames, (unsigned long)edgeDrops, (unsigned long)resyncs);
  logf("[HID] GAP=%u SENT_ATTEMPTS=%lu PENDING=%u PEAK=%u DROP=%lu STALE=%lu AVG_Q=%lu MAX_Q=%lu MAX_CALL_US=%lu\n",
      hidGapMs, (unsigned long)hidSent, (hidW-hidR)&HID_MASK, maxQueue, (unsigned long)hidDrops,
      (unsigned long)staleRecoveries, (unsigned long)avg, (unsigned long)maxQueueWaitMs, (unsigned long)maxHidCallUs);
  logf("[HID] HELD=%u REPEATS_FILTERED=%lu WARM=%d WARM_REPORTS=%lu UNMAPPED=%lu\n", physicalHeldCount,
      (unsigned long)duplicateMakes, warmReportsEnabled, (unsigned long)warmReports, (unsigned long)unmapped);
  printLEDStats();
  logf("[KEEP] ENABLED=%d AWAKE=%d ACTIVE=%d OTA_HOLD=%d TASK=%d PULSES=%lu IDLE=%lu s LIMIT=%lu s\n",
      power.enabled, power.awake, power.active, power.otaHold, powerTaskRunning, (unsigned long)power.pulses,
      (unsigned long)(uint32_t(millis()-power.lastActivityMs)/1000), (unsigned long)(IDLE_TIMEOUT_MS/1000));
  logf("[OTA] REQUEST=%d MODE=%d BUSY=%d PROGRESS=%u ERROR=%d\n", otaRequested, otaMode, otaBusy, otaProgress, lastOtaError);
  logf("[SYS] CPU=%lu MHz HEAP=%lu MIN_HEAP=%lu WDT=%d TIMEOUT=%lu LOG_DROP=%lu\n",
      (unsigned long)getCpuFrequencyMhz(), (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getMinFreeHeap(), watchdogEnabled, (unsigned long)watchdogTimeoutMs, (unsigned long)logDrops);
}
void printHelp() {
  logLine("s:estadisticas h:ayuda d:debug teclas");
  logLine("1:HID20ms 2:12ms 3:8ms 4:4ms 5:2ms");
  logLine("e:ECHO l:LED a:LED automatico b:reiniciar BLE r:reiniciar ESP32");
  logLine("k:keep-alive CPU on/off p:ventana CPU manual z:informes BLE vacios on/off");
  logLine("o:entrar OTA web x:salir OTA; Ctrl+Alt+F12 3s y SOLTAR; navegador http://192.168.4.1");
}
void serialCommands() {
  // Presupuesto acotado: un flujo serie continuo no puede monopolizar loop().
  for (unsigned n = 0; n < 8 && Serial.available(); ++n) {
    const char c = static_cast<char>(Serial.read());
    if (c == 's') printStats();
    else if (c == 'h') printHelp();
    else if (c == 'x') {
      otaRequested = false; resetOtaChord(true);
      if (otaMode) stopOTA();
    }
    else if (otaMode) continue; // no reiniciar BLE/ESP a mitad de una OTA
    else if (c == 'o') requestOTA();
    else if (c == 'd') { debugKeys = !debugKeys; logf("[DEBUG] %d\n", debugKeys); }
    else if (c >= '1' && c <= '5') {
      const uint8_t gaps[] = {20,12,8,4,2}; hidGapMs = gaps[c-'1'];
      logf("[HID] GAP=%u ms\n", hidGapMs);
    }
    else if (c == 'z') { warmReportsEnabled = !warmReportsEnabled; logf("[BLE] WARM=%d\n", warmReportsEnabled); }
    else if (c == 'k') {
      portENTER_CRITICAL(&powerMux);
      powerState.enabled = !powerState.enabled;
      powerState.active = false; powerState.restStartedMs = millis();
      const bool enabled = powerState.enabled;
      portEXIT_CRITICAL(&powerMux);
      logf("[KEEP] ENABLED=%d\n", enabled);
    }
    else if (c == 'p') {
      // Diagnóstico explícito: una ventana de carga, sin renovar la actividad de teclado.
      portENTER_CRITICAL(&powerMux);
      const bool allowed = powerState.awake;
      if (allowed) { powerState.enabled = true; powerState.pulseRequested = true; }
      portEXIT_CRITICAL(&powerMux);
      logLine(allowed ? "[KEEP] Ventana manual solicitada." : "[KEEP] Inactivo: pulsar una tecla primero.");
    }
    else if (c == 'e') { wantEcho = true; ledTrackPending(millis()); }
    else if (c == 'l') { wantLED = true; ledTrackPending(millis()); }
    else if (c == 'a') {
      autoLED = !autoLED;
      if (autoLED) { ledKnown = false; ledTrackPending(millis()); }
      logf("[LED] AUTO=%u\n", autoLED);
    }
    else if (c == 'b' && !ledRecoveryPendingForOTA() && !otaRequested) {
      keyboard.releaseAll(); keyboard.end(); resetLEDConnection(false); clearHIDQueue();
      keyboard.onLEDChange(ledChangedFromHost);
      keyboard.onPairingComplete(pairingDone);
      keyboard.begin();
    }
    else if (c == 'r') { feedWatchdog(); ESP.restart(); }
  }
}

// ================= SETUP Y LOOP =================
void setup() {
  Serial.begin(115200); // Sin esperar a que haya monitor USB conectado.
  const bool cpuSet = setCpuFrequencyMhz(CPU_TARGET_MHZ);
  const esp_reset_reason_t cause = esp_reset_reason();
  logf("\nHP KB9970 V%s LED OTA: %s\n", FW_VERSION, resetName(cause));
  logf("[CPU] Objetivo=%lu MHz actual=%lu MHz ajuste=%s\n",
       (unsigned long)CPU_TARGET_MHZ, (unsigned long)getCpuFrequencyMhz(),
       cpuSet ? "OK" : "FALLO; se conserva frecuencia disponible");
  vTaskPrioritySet(nullptr, 2); // El loop del teclado tiene prioridad sobre carga CPU.
  initPowerState(millis());
  pinMode(PIN_DATA, INPUT_PULLUP); pinMode(PIN_CLOCK, INPUT_PULLUP);
  setupWatchdog();
  powerTaskRunning = xTaskCreate(powerTask, "kb-keep", 3072, nullptr, 1, &powerTaskHandle) == pdPASS;
  if (!powerTaskRunning) logLine("[KEEP] Sin memoria para tarea: respaldo en loop, OTA deshabilitada.");

  const bool otaBoot = consumeOTABootRequest();
  if (otaBoot && startOTABootMode()) return; // Ningun BLE ni ISR PS/2 en este arranque.

  if (cause == ESP_RST_TASK_WDT || cause == ESP_RST_INT_WDT || cause == ESP_RST_WDT ||
      cause == ESP_RST_PANIC || cause == ESP_RST_BROWNOUT) {
    commandError("reinicio anormal: verificar enlace PS/2");
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  keyboard.setLogLevel(HIDLogLevel::Off);
  keyboard.setSecurityMode(HIDSecurity::JustWorks);
  keyboard.setConnIntervalMs(BLE_INTERVAL_REQUEST_MS);
  keyboard.onPairingComplete(pairingDone);
  keyboard.onLEDChange(ledChangedFromHost);
  keyboard.begin();
  feedWatchdog();
  attachInterrupt(digitalPinToInterrupt(PIN_CLOCK), clockISR, FALLING);
  ps2ReceiverAttached = true;
  logLine("[CONFIG] V2.0.0 DATA=4 CLOCK=3; HID4ms; LED recuperable; keep-alive20min; WEB bajo demanda.");
  printHelp();
}
void loop() {
  if (otaMode) {
    serviceOTA();
    serialCommands();
  } else {
    // La conexión se atiende ANTES de aceptar teclas: no borrar la primera al reconectar.
    serviceConnection();
    serviceHostLEDState();
    servicePS2();
    serviceHID();
    serviceOtaShortcut();
    serviceCommands();
    serialCommands();
    serviceOTA();
    if (!otaMode) serviceWarmReport();
  }
  if (!powerTaskRunning) cpuKeepAliveSlice(); // recuperación de falta de memoria
  portENTER_CRITICAL(&pairingMux);
  const int8_t pairing = pairingStatus; pairingStatus = -1;
  portEXIT_CRITICAL(&pairingMux);
  if (pairing != -1) logf("[BLE] PAIRING=%s\n", pairing ? "OK" : "ERROR");
  serviceLogOutput();
  feedWatchdog();
  delay(1); // ceder a radio/tareas; no espera por evento ni cálculo intensivo aquí
}
