/**
 * ============================================================================
 * PROYECTO: Monitor OLED + Puente BLE para Timemore Black Mirror Dot
 * ARCHIVO:  firmware.ino
 * PLACA:    WeAct Studio ESP32-S3-A (ESP32-S3 Core Board)
 * FUENTE:   https://github.com/jcallano/Timemore_Dot_OLED
 * LICENCIA: Copyright (c) 2026 jcallano. Creative Commons Reconocimiento 3.0 Unported
 *           (CC BY 3.0, https://creativecommons.org/licenses/by/3.0/). Se permite usar,
 *           modificar y redistribuir citando la fuente (autor y URL de arriba). Ver LICENSE.
 * ============================================================================
 *
 * PINOUT OFICIAL WEACT STUDIO ESP32-S3-A (ESP32_S3_A_Sch.pdf):
 * ----------------------------------------------------------------------------
 * - LED RGB Direccionable (WS2812B integrado): GPIO 48
 * - Botón BOOT integrado:                      GPIO 0  (Activo en LOW)
 * - Botón extra / Usuario (en algunas rev.):   GPIO 45
 * - Bus I2C por defecto en ESP32-S3 (OLED):    SDA = GPIO 8, SCL = GPIO 9
 * - Alimentación OLED (VCC / GND):             3V3 y GND de la placa WeAct
 *
 * PANTALLA OLED 128x64 I2C (driver SSD1309): módulo 2.42" "2.42OLED-IIC v1.1"
 * (también funciona con el 1.54" M154-4P). Dirección 0x3C (serigrafía 0x78).
 * Bus I2C hardware configurado a 800 kHz; un volcado completo de 1 KB tarda
 * del orden de 12 ms (ver AUDITORIA_FIRMWARE.md, M4).
 *
 * FUNCIONES:
 * 1. Peso en tiempo real (0.1 g) desde la Timemore Dot por BLE (protocolo
 *    Open Scale 0xFFF0 -> 0xFFF1, tramas A5 5A) con tara por software.
 * 2. Caudal (g/s): ventana deslizante + mediana de 3 + EMA adaptativa.
 * 3. Cronómetro con Auto-Brew (arranca al detectar vertido tras la tara).
 * 4. Ratio 1:X con la dosis, historial de 10 pour-overs (PSRAM + NVS) y actualización por Bluetooth.
 *
 * CONTROLES (botón del encoder GPIO 12, o BOOT GPIO 0 con los mismos clics):
 * ----------------------------------------------------------------------------
 * - 1 clic:   Cronómetro inicio / pausa (al pausar se guarda el pour-over)
 * - 2 clics:  TARA + reinicio del cronómetro
 * - 3 clics o mantener 1.5 s: abrir / cerrar el menú
 * - Girar:    cambiar de vista (en el menú: mover cursor o ajustar la dosis)
 * - BOOT mantenido >= 4 s: borrar la báscula vinculada (re-vincular)
 *
 * Sin librerías externas de pantalla: motor gráfico propio sobre <Wire.h>.
 * Solo requiere la librería "NimBLE-Arduino". Particiones: partitions.csv (8 MB).
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_chip_info.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#if __has_include(<driver/pulse_cnt.h>)
  #include <driver/pulse_cnt.h>
  #define HAS_ESP32S3_HW_PCNT 1
#endif
#include <math.h>
#include <string.h>
#include <stdarg.h>

// Módulos puros (sin Arduino ni estado global), probados con el autotest ('X' por el monitor serie)
#include "src/timemore_protocol.h"
#include "src/flow_engine.h"
#include "src/selftest.h"
#include "src/bookoo_protocol.h"
#include "src/difluid_protocol.h"
#include "src/tds_sampler.h"

// ============================================================================
// WRAPPER USB-SERIAL/JTAG (HWCDC) 100% NO BLOQUEANTE CUANDO EL MONITOR ESTÁ CERRADO
// ============================================================================
// En el ESP32-S3 con USBMode=hwcdc, cuando el Monitor Serie en la PC está cerrado
// (o al alimentar por batería), ningún host USB drena el FIFO TX de 64 bytes del
// periférico USB-Serial/JTAG y las llamadas estándar a Serial.printf/println bloquean
// la CPU hasta 100 ms por cada paquete. Este wrapper configura setTxTimeoutMs(0) y
// descarta en 0 us cualquier escritura cuando el FIFO TX no está libre, eliminando
// por completo el lag o jitter en la interfaz OLED y el encoder.
struct NonBlockingUsbSerial {
  void begin(unsigned long baud) {
    ::Serial.begin(baud);
#if defined(ESP_ARDUINO_VERSION_MAJOR)
    ::Serial.setTxTimeoutMs(0);
#endif
  }
  int available() {
    return ::Serial.available();
  }
  int read() {
    return ::Serial.read();
  }
  inline bool canWrite() const {
    return static_cast<bool>(::Serial) && (::Serial.availableForWrite() >= 64);
  }
  void print(const char* s) {
    if (s && canWrite()) {
      ::Serial.write(reinterpret_cast<const uint8_t*>(s), strlen(s));
    }
  }
  void println(const char* s = "") {
    if (!canWrite()) return;
    if (s && *s) {
      ::Serial.write(reinterpret_cast<const uint8_t*>(s), strlen(s));
    }
    ::Serial.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
  }
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (!canWrite()) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) {
      size_t len = (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n) : (sizeof(buf) - 1);
      ::Serial.write(reinterpret_cast<const uint8_t*>(buf), len);
    }
  }
};
static NonBlockingUsbSerial SafeUsbSerial;
#undef Serial  // el core define Serial como HWCDCSerial; se sustituye a propósito por el envoltorio no bloqueante
#define Serial SafeUsbSerial

// ============================================================================
// DETECCIÓN AUTOMÁTICA DE VERSIÓN DE NIMBLE-ARDUINO (v1.4.x vs v2.x)
// ============================================================================
#if defined(NIMBLE_ARDUINO_2_x) || (defined(CONFIG_NIMBLE_CPP_IDF) && !defined(ESP_NIMBLE_CPP_VERSION))
  #define NIMBLE_V2 1
#elif __has_include(<NimBLEConnInfo.h>)
  #define NIMBLE_V2 1
#endif

// ============================================================================
// CONFIGURACIÓN DE HARDWARE: WEACT STUDIO ESP32-S3-A + PANTALLA OLED I2C
// ============================================================================
// En la placa WeAct Studio ESP32-S3-A, el LED RGB WS2812B integrado está en GPIO 48.
// (Nota: pins_arduino.h de esp32s3 define la macro #define PIN_RGB_LED 48, por lo que
//  usamos WEACT_RGB_LED_PIN para evitar colisión de macros del preprocesador).
static constexpr uint8_t WEACT_RGB_LED_PIN    = 48;
static constexpr uint8_t PIN_BOOT_BUTTON      = 0;   // Botón BOOT integrado (LOW al presionar)

// Pines I2C para la pantalla OLED M154-4P (1.54" SSD1309 128x64)
static constexpr int PIN_OLED_SDA             = 8;
static constexpr int PIN_OLED_SCL             = 9;
static constexpr uint32_t OLED_I2C_FREQ_HZ    = 1000000UL; // 1 MHz Fast-Mode Plus (a 800 kHz el volcado completo tardaba 16 ms)

// Dirección I2C en la serigrafía del módulo M154-4P:
// La serigrafía marca "0x78" (dirección de 8 bits con bit R/W=0).
// En Arduino <Wire.h> las direcciones son de 7 bits: (0x78 >> 1) = 0x3C.
static constexpr uint8_t OLED_SILKSCREEN_ADDR = 0x78;
static constexpr uint8_t OLED_I2C_ADDR_7BIT   = (OLED_SILKSCREEN_ADDR >= 0x78)
                                                  ? (OLED_SILKSCREEN_ADDR >> 1)
                                                  : OLED_SILKSCREEN_ADDR; // 0x3C

// En el controlador SSD1309 (M154-4P 1.54" 128x64), la RAM es de 128x64 exactos (colOffset = 0)
static constexpr bool OLED_IS_SH1106          = false;

// ============================================================================
// GESTIÓN DE BATERÍA CON MÓDULO HW-107 (TP4056 + PROTECCIÓN DW01A)
// ============================================================================
// Cableado de alimentación HW-107 -> WeAct Studio ESP32-S3-A:
// - OUT+ del HW-107 -> Pin "5V" (VIN del regulador LDO 3.3V de la placa WeAct)
// - OUT- del HW-107 -> Pin "GND" de la placa WeAct
// - Pantalla OLED M154-4P alimentada desde el pin regulado "3V3" de la WeAct.
//
// Telemetría de voltaje de batería (Opcional pero recomendado):
// - Divisor resistivo de 2 resistencias iguales (ej. 100kΩ + 100kΩ) entre OUT+ y GND,
//   con el punto medio conectado a GPIO 4 (ADC1_CH3).
// - Pin opcional CHRG (Pin 7 del TP4056 en el HW-107, activo en LOW al cargar) -> GPIO 5.
static constexpr int PIN_BATT_ADC             = 4;     // GPIO 4 (ADC1) para medir voltaje de OUT+
static constexpr int PIN_TP4056_CHRG          = 5;     // GPIO 5 (opcional) al pin 7 CHRG del TP4056
static constexpr float BATT_DIVIDER_RATIO     = 2.0f;  // Divisor 100k / 100k = factor 2.0
static constexpr float BATT_CUTOFF_VOLTS      = 3.25f; // Corte por software (deep sleep) antes del DW01A a 2.4V

// Ahorro de energía para batería (Auto-Dimming OLED y reposo en light sleep)
static constexpr uint32_t AUTO_DIM_TIMEOUT_MS   = 120000UL; // 2 min sin actividad -> Atenuar OLED
static constexpr uint32_t AUTO_SLEEP_TIMEOUT_MS = 600000UL; // 10 min sin actividad -> Reposo (light sleep)
static constexpr uint32_t BATT_SLEEP_CHECK_MS   = 600000UL; // En reposo: despertar cada 10 min a medir la batería

// ============================================================================
// ENCODER ROTATORIO CON BOTÓN PUSH (EC11 / KY-040 / SMD THUMBWHEEL)
// ============================================================================
// Pines RTC-GPIO en WeAct Studio ESP32-S3-A (todos con INPUT_PULLUP interno):
// - ENC_CLK (Fase A): GPIO 10
// - ENC_DT  (Fase B): GPIO 11
// - ENC_SW  (Push):   GPIO 12 (Activo en LOW; también despierta del Deep Sleep!)
// - GND / Común:      GND
// - VCC / + (si aplica): 3V3
static constexpr int PIN_ENC_CLK              = 10;
static constexpr int PIN_ENC_DT               = 11;
static constexpr int PIN_ENC_SW               = 12;

// Pasos del encoder por "detent" (muesca mecánica): típicamente 4 (o 2 en encoders de media onda)
static constexpr int ENC_STEPS_PER_DETENT     = 4;

// Brillo del LED RGB WS2812 integrado (0..255)
static constexpr uint8_t LED_BRIGHTNESS       = 25;

// Intervalos de actualización y tiempos
static constexpr uint32_t KEEP_ALIVE_MS       = 25000; // Pulso Anti Auto-Off cada 25 s
static constexpr uint32_t SCAN_DURATION_SEC   = 3;     // Escaneo BLE de 3 s
static constexpr uint32_t SCAN_RETRY_MS       = 1500;
static constexpr uint32_t SERIAL_STATUS_MS    = 5000;

// Botón BOOT (GPIO 0): mismos multiclic que el encoder; mantenerlo >= 4 s borra la MAC (re-vincular)
static constexpr uint32_t BTN_NVS_RESET_MS    = 4000;

// Rango de la dosis de café (menú y valor cargado de NVS)
static constexpr float DOSE_MIN_G             = 5.0f;
static constexpr float DOSE_MAX_G             = 60.0f;

// ============================================================================
// UUIDS BLE DE LA TIMEMORE BLACK MIRROR DOT (TODOS LOS PERFILES GATT)
// ============================================================================
// Perfil 1: Timemore UART Transparente (0xFFE0 -> Notify 0xFFE1, Write 0xFFE2/0xFFE1)
static constexpr uint16_t UUID16_TIMEMORE_SVC_FFE0  = 0xFFE0;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFE1  = 0xFFE1;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFE2  = 0xFFE2;

// Perfil 2: Timemore Open Scale Protocol (0xFFF0 -> Notify 0xFFF1, Write 0xFFF2)
static constexpr uint16_t UUID16_TIMEMORE_SVC_FFF0  = 0xFFF0;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFF1  = 0xFFF1;
static constexpr uint16_t UUID16_TIMEMORE_CHR_FFF2  = 0xFFF2;

// Perfil 3: Bluetooth SIG Weight Scale (0x181D -> Notify 0x2A9D)
static constexpr uint16_t UUID16_TIMEMORE_SVC_181D  = 0x181D;
static constexpr uint16_t UUID16_TIMEMORE_CHR_2A9D  = 0x2A9D;

// Perfil 4: Environmental Sensing / Timemore Dot (0x181A -> Notify 0x2A6D, Write 0x2A6E)
static constexpr uint16_t UUID16_TIMEMORE_SVC_181A  = 0x181A;
static constexpr uint16_t UUID16_TIMEMORE_CHR_2A6D  = 0x2A6D;
static constexpr uint16_t UUID16_TIMEMORE_CHR_2A6E  = 0x2A6E;

// ============================================================================
// VARIABLES GLOBALES
// ============================================================================
Preferences preferences;
static const char* NVS_NAMESPACE = "tmdot_oled";
static const char* NVS_KEY_MAC   = "dot_mac";
static const char* NVS_KEY_TYPE  = "dot_type";

char savedTimemoreMac[20]        = {0};
uint8_t savedTimemoreAddrType    = 0;
bool hasSavedMac                 = false;

// Estado de escaneo y conexión BLE Central
volatile bool candidateFound     = false;
char candidateMac[20]            = {0};
uint8_t candidateAddrType        = 0;
int candidateRSSI                = -999;

volatile bool scaleConnected     = false;
volatile bool doConnectToScale   = false;
volatile bool pendingStopScan    = false;
volatile bool isScanning         = false;
unsigned long lastScanAttemptMs  = 0;

NimBLEClient* pScaleClient                   = nullptr;
NimBLERemoteCharacteristic* pScaleNotifyChar = nullptr;
NimBLERemoteCharacteristic* pScaleWriteChar  = nullptr;

// Máquina de estados de la conexión BLE no bloqueante (ver serviceBleConnection())
enum BleConnectPhase : uint8_t { BLE_IDLE = 0, BLE_CONNECTING, BLE_LINK_UP, BLE_LINK_FAILED };
volatile uint8_t bleConnectPhase             = BLE_IDLE;
volatile int bleConnectFailReason            = 0;
unsigned long bleConnectStartMs              = 0;
uint8_t bleConnectAddrType                   = 0;
bool bleConnectTriedAlt                      = false;
volatile bool bleDisconnectEvent             = false; // onDisconnect -> loop() limpia el estado de caudal
uint8_t batteryQueryTries                    = 0;
uint8_t connItvlFixAttempts                  = 0;     // Reintentos para bajar el intervalo de conexión
unsigned long lastConnItvlCheckMs            = 0;

// Cola de salida de la Dot (ver DOT_COLA_BLE.md / COLA_DOT_MTU.md). La Dot envía como máximo una
// notificación cada 100 ms; con el MTU por defecto (23) solo cabe una trama de peso por notificación y
// cada trama extra (respuesta a un comando, aviso de batería espontáneo cada ~30 s) retrasa el peso
// 100 ms PARA SIEMPRE. Con un MTU grande la Dot junta varias tramas en una notificación y vacía la cola.
static constexpr uint16_t DOT_PREFERRED_MTU  = 247;
static constexpr uint32_t MTU_EXCHANGE_DELAY_MS = 1000UL; // después de suscribirse, no durante connect()
unsigned long pendingMtuExchangeAtMs         = 0;
uint8_t mtuExchangeTries                     = 0;
volatile uint16_t scaleLinkMtu               = 23;
volatile uint32_t multiFramePackets          = 0;     // notificaciones con > 1 trama (la Dot vaciando su cola)
volatile uint32_t openScalePackets           = 0;
volatile unsigned long queueProbeSentMs      = 0;     // sonda de cola 02 08 en vuelo (0 = ninguna)
volatile uint32_t queueProbeLastMs           = 0;     // último tiempo sonda -> respuesta = tamaño de la cola
volatile bool queueProbeResultReady          = false;
unsigned long lastKeepAliveSentMs            = 0;     // último sondeo realmente enviado
static constexpr uint32_t KEEP_ALIVE_FORCE_MS = 50000UL; // la Dot se apaga sola a los ~70 s sin comandos

// Guardado diferido de la MAC: solo tras recibir peso real de la báscula recién conectada
volatile uint32_t weightPacketsRx            = 0;     // Paquetes con peso o trama Open Scale válida
unsigned long scaleConnectedAtMs             = 0;
bool pendingScaleSave                        = false;
char pendingSaveMac[20]                      = {0};
uint8_t pendingSaveAddrType                  = 0;
static constexpr uint32_t NEW_SCALE_PROOF_MS = 10000UL; // Sin peso en 10 s -> no es una Timemore

// Telemetría de Peso, Caudal (Flow Rate) y Batería
volatile float rawWeightGrams            = 0.0f;
volatile float softwareTareOffset        = 0.0f;
volatile float currentWeightGrams        = 0.0f;
volatile float scaleNativeFlowGps        = 0.0f;
volatile uint8_t scaleBatteryPct         = 0;
volatile uint8_t scaleBatteryBars        = 0;
volatile bool scaleBatteryValid          = false;
unsigned long pendingBatteryQueryAtMs    = 0;
volatile uint32_t packetsRxCount         = 0;

// Motor de caudal (src/flow_engine.*). Su estado lo modifica SOLO loop() (ver drainWeightQueue()).
// Los alias mantienen los nombres que usan la UI, el auto-timer, el LED y la gestión de energía.
FlowEngine flowEngine;
float& currentFlowRateGps                = flowEngine.currentGps;
float& displayedFlowRateGps              = flowEngine.displayedGps;
float& peakFlowRateGps                   = flowEngine.peakGps;
bool& weightIsStable                     = flowEngine.stable;
unsigned long& lastWeightStepChangeMs    = flowEngine.lastStepMs;
unsigned long& prevFlowCalcMs            = flowEngine.lastCalcMs;

// Cronómetro de Extracción (Manual + Auto-Brew) y Protección Post-Tara
bool timerRunning                        = false;
bool autoTimerEnabled                    = true;  // Preferencia del usuario en Menú (2.AUTO-TMR: ON/OFF)
bool autoTimerArmed                      = true;  // Se arma automáticamente al hacer Tara si autoTimerEnabled=true
volatile unsigned long lastTareCommandMs = 0;     // Timestamp del último comando de Tara para descartar paquetes pre-tara en vuelo
volatile bool tareSettlingActive         = false; // true mientras esperamos que la Timemore Dot confirme 0.0 g tras hacer Tara
volatile bool tareZeroConfirmed          = true;  // true cuando la báscula ya pasó por 0.0 g tras la Tara
volatile bool tareZeroAmbiguous          = false; // Bruto cayó a 0 con tara activa: ¿se levantó la jarra o fue la tara física?
unsigned long timerStartMillis           = 0;
unsigned long timerAccumulatedMs         = 0;
// Auto-stop: el peso cae < 70 % del máximo de la extracción durante AUTO_STOP_MS
static constexpr uint32_t AUTO_STOP_MS   = 5000UL;
float brewMaxWeightGrams                 = 0.0f;
unsigned long autoStopSinceMs            = 0;
uint16_t autoStopSampleIdx               = 0;

// Banderas diferidas de control
volatile bool pendingScaleTare           = false;

// Botón BOOT y LED RGB
bool bootBtnPressed                      = false;
bool bootBtnLongHandled                  = false;
unsigned long bootBtnDownMs              = 0;
unsigned long ledFlashUntilMs            = 0;
uint8_t cachedR = 255, cachedG = 255, cachedB = 255;

// Temporizadores de bucle y gestión de energía (HW-107 TP4056)
unsigned long lastOledUpdateMs           = 0;
volatile bool uiNeedsImmediateRedraw     = false;
unsigned long lastKeepAliveMs            = 0;
unsigned long lastSerialStatusMs         = 0;
unsigned long lastBattReadMs             = 0;
volatile unsigned long lastActivityMs    = 0;
bool oledIsDimmed                        = false;

// Estado de la batería local del monitor (HW-107 / TP4056)
bool localBattSensorPresent              = false;
bool battAdcProbedAtBoot                 = false;
float localBattVolts                     = 4.0f;
uint8_t localBattPct                     = 100;
bool localBattCharging                   = false;
bool localBattHasReading                 = false; // Primera lectura válida del divisor ya tomada
uint32_t battProbePullDownMv             = 0;     // Diagnóstico de la sonda del divisor (GPIO 4)
uint32_t battProbePullUpMv               = 0;
volatile uint32_t connParamReqCount      = 0;     // Peticiones de parámetros de conexión de la Dot
volatile uint16_t lastConnReqMin         = 0;
volatile uint16_t lastConnReqMax         = 0;
volatile uint16_t lastConnReqLat         = 0;
uint8_t lowBattReadings                  = 0;     // Lecturas seguidas por debajo de BATT_CUTOFF_VOLTS
float prevActivityWeightGrams            = 0.0f;
volatile unsigned long lastBleWeightRxMs = 0;

// ============================================================================
// VARIABLES DEL ENCODER ROTATORIO Y VISTAS / MENÚ EN PANTALLA OLED
// ============================================================================
volatile int32_t encRawPulses            = 0;
volatile uint8_t encPrevState            = 0x03;
int32_t lastEncPulseCount                = 0;     // Último valor leído del contador PCNT
int32_t encPulseAccum                    = 0;     // Pulsos acumulados hacia la siguiente muesca

bool encSwPressed                        = false;
bool encSwLongHandled                    = false;
unsigned long encSwDownMs                = 0;
uint8_t encClickCount                    = 0;
unsigned long encLastReleaseMs           = 0;

uint8_t bootClickCount                   = 0;
unsigned long bootLastReleaseMs          = 0;

// Ventana máxima entre clics consecutivos para detectar Single / Double / Triple Click
static constexpr unsigned long MULTI_CLICK_GAP_MS = 280UL;

// Vistas de la pantalla principal (se cambian girando el encoder; TODAS muestran el PESO en grande):
// 0 = PESO + RATIO 1:X    (PRINCIPAL POR DEFECTO: Flow arriba + Peso grande 3x + Dosis + Ratio 1:X.X + Cronómetro)
// 1 = SOLO PESO GIGANTE   (Peso gigante 4x a pantalla completa + indicador de estabilidad)
// 2 = PESO + FLOW + TIMER (Peso grande 3x arriba + Barra de flujo + Caudal g/s + Cronómetro abajo)
// 3 = PESO + GRAFICA FLOW (Peso grande 3x arriba + Curva histórica en vivo de Caudal g/s abajo)
uint8_t currentScreenView                = 0;
static constexpr uint8_t NUM_SCREEN_VIEWS = 4;
static constexpr uint8_t BOOKOO_SCREEN_VIEW = NUM_SCREEN_VIEWS;  // vista extra "modo puente": solo con la máquina conectada

// Estado de la interfaz: un solo valor en lugar de 3 booleanos (inSettingsMenu, editingDoseInMenu,
// inPourOverViewer) que podían quedar en combinaciones imposibles.
//   Main         -> vistas de pesaje (el giro cambia de vista)
//   Menu         -> menú de ajustes (3 clics); el giro mueve el cursor
//   MenuEdit     -> editando un valor del menú (dosis, ratio, tiempo); el giro lo ajusta
//   PourViewer   -> visor del historial (pour-overs o shots, según viewerShowsShots)
//   TdsMeasure   -> medida de TDS con el DiFluid R2 (58_difluid_r2.ino) para un registro del historial
enum class UiState : uint8_t { Main, Menu, MenuEdit, PourViewer, TdsMeasure };
UiState uiState                          = UiState::Main;

// Modo de preparación (Menú -> 1. Modo). Se guarda en la NVS.
enum class BrewMode : uint8_t { PourOver = 0, Espresso = 1 };
BrewMode brewMode                        = BrewMode::PourOver;

// Menú interactivo controlado con el Encoder (se abre con 3 clics en ENC_SW). Sus opciones dependen del
// modo: buildMenuItems() (80_pantalla.ino) rellena la lista con estos identificadores.
enum MenuItemId : uint8_t {
  MI_MODE, MI_HISTORY, MI_AUTOTIMER, MI_DOSE, MI_RATIO, MI_TARGET_TIME,
  MI_BRIGHT, MI_BRIDGE, MI_RELINK, MI_UPDATE, MI_SLEEP, MI_EXIT, MI_TDS, MI_JAR, MI_R2ZERO
};
static constexpr uint8_t MENU_MAX_ITEMS  = 15;
int8_t menuSelectedIndex                 = 0;

// Parámetros de barista configurables desde el Encoder
float coffeeDoseGrams                    = 15.0f; // Dosis de café seco del Pour-Over (ratio 1:X)
uint8_t oledBrightnessLevel              = 2;     // 0=Bajo (0x15), 1=Medio (0x6F), 2=Alto (0xCF)

// Receta de espresso (Menú en modo espresso). Peso objetivo = dosis x ratio.
float espDoseGrams                       = 18.0f;
float espRatio                           = 2.0f;
uint8_t espTargetSec                     = 28;
float espRebaseAvgGrams                  = 2.0f;  // lo que sigue cayendo tras cortar (aprendido de los shots)
static constexpr float ESP_DOSE_MIN_G    = 5.0f;
static constexpr float ESP_DOSE_MAX_G    = 30.0f;
static constexpr float ESP_RATIO_MIN     = 1.0f;
static constexpr float ESP_RATIO_MAX     = 4.0f;
static constexpr uint8_t ESP_TIME_MIN_S  = 10;
static constexpr uint8_t ESP_TIME_MAX_S  = 90;

// Shot de espresso en curso (45_espresso.ino). También lo usan los shots que arranca la cafetera por el
// puente BooKoo, en cualquier modo.
enum class ShotPhase : uint8_t { Idle, Running, Settling };
ShotPhase shotPhase                      = ShotPhase::Idle;
unsigned long shotStartMs                = 0;     // inicio del cronómetro del shot
unsigned long shotFirstDropMs            = 0;     // 0 = todavía no ha caído la primera gota
bool shotPiKnown                         = false; // false: arrancó con la primera gota (sin preinfusión medible)
float shotWeightAtStop                   = 0.0f;
bool shotByMachine                       = false;
bool shotAutoStopped                     = false;
bool bannerIsShot                        = false; // el aviso de guardado es de un shot (no de un pour-over)
bool viewerShowsShots                    = false; // el visor del historial muestra shots

// Buffer circular de 64 muestras para la gráfica en vivo de Flow Rate (Vista 3)
uint8_t flowGraphHistory[64]             = {0};
uint8_t flowGraphHead                    = 0;
unsigned long lastFlowGraphSampleMs      = 0;

// ============================================================================
// ALMACENAMIENTO DE POUR-OVERS EN PSRAM + PERSISTENCIA EN FLASH NVS
// ============================================================================
static constexpr uint8_t MAX_SAVED_POURS         = 10;   // Hasta 10 Pour-Overs completos guardados
static constexpr uint16_t MAX_LIVE_POUR_SAMPLES  = 1200; // 10 min a 2 Hz (cada 500 ms) en PSRAM
static constexpr uint8_t POUR_GRAPH_POINTS       = 120;  // 120 columnas para graficar en OLED 128x64

struct LivePourSample {
  float weightGrams;
  float flowGps;
};

struct PourOverRecord {
  uint32_t magic;                      // 0x504F5552 ("POUR")
  uint16_t pourId;                     // Número secuencial (#1, #2...)
  uint16_t durationSec;                // Duración total en segundos
  float doseGrams;                     // Café seco (g)
  float finalWeightGrams;              // Agua total / bebida final (g)
  float finalRatio;                    // Ratio alcanzado (ej. 16.7 para 1:16.7)
  float avgFlowGps;                    // Caudal promedio (g/s)
  float peakFlowGps;                   // Caudal máximo (g/s)
  uint8_t poursCount;                  // Número de vertidos/pulsos detectados
  uint8_t weightCurve[POUR_GRAPH_POINTS]; // Curva de peso normalizada (0..40 px)
  uint8_t flowCurve[POUR_GRAPH_POINTS];   // Curva de caudal normalizada (0..40 px)
  // 1 = el primer vertido fue un bloom (poursCount lo incluye: se muestra "BLOOM+<poursCount-1>").
  // Ocupa un byte del relleno final: sizeof no cambia (272) y los registros antiguos de la NVS, que se
  // guardaron con memset a 0, se leen como "sin bloom".
  uint8_t hasBloom;
};
static_assert(sizeof(PourOverRecord) == 272, "PourOverRecord cambió de tamaño: invalidaría el historial guardado en la NVS");

// Punteros alojados dinámicamente en PSRAM (con fallback automático a SRAM si PSRAM está deshabilitada)
LivePourSample* psramLiveSamples         = nullptr;
PourOverRecord* psramSavedPours          = nullptr;
bool hasPsramHardware                    = false;
uint32_t totalPsramBytes                 = 0;
uint32_t totalFlashBytes                 = 0;

uint16_t liveSampleCount                 = 0;
unsigned long lastLiveSampleMs           = 0;
uint8_t currentPourPulsesCount           = 0;
bool wasPouringActive                    = false;

uint8_t savedPoursCount                  = 0;
uint16_t nextPourSequenceId              = 1;
uint16_t activeRecordingPourId           = 0;     // pourId ya guardado de la grabación en curso (0 = aún no guardada)

// Demostración en vivo de un V60 (orden serie 'v'): loop() inyecta el peso simulado en la cola de muestras
// y se ignora el de la Dot. El pour-over resultante se guarda solo en RAM.
volatile bool demoLiveActive             = false;

// Estado del Visor Interactivo de Pour-Overs en la OLED (activo con uiState == UiState::PourViewer)
bool pourViewerShowStatsPage             = false; // false = Gráfica Peso+Flow+Ratio, true = Ficha Técnica
int8_t selectedPourSlot                  = 0;
unsigned long pourSavedBannerUntilMs     = 0;
uint16_t lastSavedPourId                 = 0;

// Muestra de peso encolada por la tarea BLE y procesada en loop() (ver 50_ble_scale.ino)
struct WeightSample {
  unsigned long ms;   // millis() de llegada del paquete
  float grams;        // peso bruto de la báscula
  bool stable;
};
static constexpr UBaseType_t WEIGHT_QUEUE_LEN = 32;   // ~3 s a 10 Hz
QueueHandle_t weightQueue                      = nullptr;
volatile uint32_t droppedWeightSamples         = 0;

void processWeightSample(const WeightSample& sample);

// Puente BooKoo (ver 55_bookoo_bridge.ino): el panel se anuncia como BooKoo Themis Ultra para que la
// máquina (Wendougee Lita/Data-S vía E-Bar) use la Dot como báscula. Preferencia en NVS ("bk_on").
bool bookooBridgeEnabled                      = true;
volatile bool bookooMachineConnected          = false; // central (máquina/app) conectado al panel
volatile bool bookooSubscribed                = false; // notificaciones de 0xFF11 activadas
volatile float bookooLatestRawGrams           = 0.0f;  // último peso bruto de la Dot (tarea BLE)
volatile uint32_t bookooPendingCmds           = 0;     // bits de comandos de la máquina para loop()
volatile uint32_t bookooFramesSent            = 0;
volatile uint32_t bookooCmdsRx                = 0;
char bookooAdvName[24]                        = "BOOKOO_SC_U 000000";

// TDS y bebida de cada registro (46_extraccion.ino)
struct BrewExtra {
  uint16_t id;           // pourId o shotId (0 = libre)
  uint16_t tdsPacked;    // tds::pack (0 = sin medida)
  uint16_t beverageX10;  // bebida medida en décimas de g (0 = sin medida; solo pour-over)
  uint16_t reserved;
};
static_assert(sizeof(BrewExtra) == 8, "BrewExtra cambió de tamaño");

// Estado de la conexión con el DiFluid R2 (58_difluid_r2.ino)
enum class R2State : uint8_t { Off, Scanning, Connecting, Ready, Measuring, Failed, Calibrating };

// Ancla para el generador de prototipos del IDE de Arduino: los prototipos automáticos de todas las
// pestañas se insertan justo antes de la PRIMERA función de esta pestaña. Si no hubiera ninguna, los
// insertaría al principio del archivo (antes de los tipos y de Arduino.h) y la compilación fallaría.
// Debe quedar al final, después de todos los tipos (PourOverRecord, WeightSample, ...).
void sketchPrototypeAnchor() {}
