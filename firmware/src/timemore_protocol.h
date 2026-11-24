// Protocolo BLE de la Timemore Black Mirror Dot: decodificación de notificaciones y
// construcción de tramas Open Scale. Módulo PURO (sin Arduino, sin estado global):
// se puede probar con tramas grabadas (ver selftest.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tmproto {

struct ParseResult {
  bool hasWeight          = false;  // el paquete trae una lectura de peso
  float grams             = 0.0f;   // peso bruto (g), si hasWeight
  bool stable             = true;
  bool hasBattery         = false;  // el paquete trae batería de la báscula
  uint8_t batteryPct      = 0;
  uint8_t batteryBars     = 0;
  bool batteryBarsValid   = false;  // solo las tramas Open Scale 0x05 traen barras
  bool hasNativeFlow      = false;  // caudal calculado por la propia báscula (Open Scale)
  float nativeFlowGps     = 0.0f;
  bool sawOpenScaleFrame  = false;  // había al menos una trama A5 5A válida (prueba de Timemore)
  uint8_t frameCount      = 0;      // tramas A5 5A válidas en el paquete (> 1: la Dot vació su cola)
  bool sawProbeReply      = false;  // respuesta a la sonda de cola (Cmd 0x08, cualquier opcode)
};

// CRC16 (polinomio 0xA001, inicial 0xFFFF) usado por el protocolo Open Scale
uint16_t crc16(const uint8_t* data, size_t length);

// Construye una trama A5 5A <opcode> <cmd> <len_hi> <len_lo> <payload> <crc_hi> <crc_lo>.
// Devuelve la longitud total o 0 si no cabe (payload máximo 32 bytes).
size_t buildFrame(uint8_t opcode, uint8_t cmd, const uint8_t* payload, uint16_t payloadLen,
                  uint8_t* out, size_t outCap);

// Decodifica una notificación. Soporta, por orden de prioridad:
//  A) Open Scale (A5 5A) en 0xFFF1, con varias tramas por paquete (se queda con la última de peso)
//  B) UART 0xFFE1 (cabecera AA 55 / 55 AA o AA/55 corta)
//  C1) 0x2A6D: 9 bytes, int32 LE en miligramos
//  C2) 0x2A9D: 4-6 bytes, int16 LE en décimas de gramo
//  D) ASCII ("  18.4 g")
// Si el paquete trae tramas Open Scale válidas sin peso, NO se prueban los casos B-D.
ParseResult parsePayload(const uint8_t* data, size_t len);

}  // namespace tmproto
