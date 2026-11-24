// ============================================================================
// ENCODER EC11 (PCNT por hardware)
// Inicialización del contador de pulsos y lectura de la posición.
// Pestaña del sketch: el IDE la concatena tras basicoledtimemore_dot.ino (orden alfabético).
// Tipos, constantes y variables globales compartidas están en la pestaña principal.
// ============================================================================

// ============================================================================
// PERIFÉRICO DE HARDWARE PCNT (PULSE COUNTER) DEL ESP32-S3 PARA EL ENCODER
// ============================================================================
#if defined(HAS_ESP32S3_HW_PCNT)
static pcnt_unit_handle_t pcntEncoderUnit = nullptr;
static pcnt_channel_handle_t pcntChanA    = nullptr;
static pcnt_channel_handle_t pcntChanB    = nullptr;
static bool hwPcntActive                  = false;
#endif

// Tabla de respaldo en caso de que PCNT no esté activo
static const int8_t DRAM_ATTR ENC_STATE_TABLE[16] = {
   0,  1, -1,  0,
  -1,  0,  0,  1,
   1,  0,  0, -1,
   0, -1,  1,  0
};

void IRAM_ATTR isrEncoderAB() {
  uint8_t clk = static_cast<uint8_t>(digitalRead(PIN_ENC_CLK));
  uint8_t dt  = static_cast<uint8_t>(digitalRead(PIN_ENC_DT));
  uint8_t curState = static_cast<uint8_t>((clk << 1) | dt);
  uint8_t idx = static_cast<uint8_t>((encPrevState << 2) | curState);
  encRawPulses = encRawPulses + ENC_STATE_TABLE[idx & 0x0F];
  encPrevState = curState;
}

void initHardwareEncoder() {
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC_SW,  INPUT_PULLUP);
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_CLK));
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_DT));
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_SW));

#if defined(HAS_ESP32S3_HW_PCNT)
  pcnt_unit_config_t unitCfg = {};
  unitCfg.low_limit  = -32000;
  unitCfg.high_limit =  32000;
  unitCfg.flags.accum_count = 1;

  if (pcnt_new_unit(&unitCfg, &pcntEncoderUnit) == ESP_OK) {
    // Filtro anti-rebote en silicio del ESP32-S3 (10000 ns = 10 us en el reloj APB)
    pcnt_glitch_filter_config_t filterCfg = {};
    filterCfg.max_glitch_ns = 10000;
    pcnt_unit_set_glitch_filter(pcntEncoderUnit, &filterCfg);

    // Canal A: flancos en CLK (GPIO 10), nivel en DT (GPIO 11) -> Decodificación X4 (Sentido horario = +)
    pcnt_chan_config_t chanACfg = {};
    chanACfg.edge_gpio_num  = PIN_ENC_CLK;
    chanACfg.level_gpio_num = PIN_ENC_DT;
    pcnt_new_channel(pcntEncoderUnit, &chanACfg, &pcntChanA);
    pcnt_channel_set_edge_action(pcntChanA,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(pcntChanA,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // Canal B: flancos en DT (GPIO 11), nivel en CLK (GPIO 10) -> Decodificación X4 (Sentido horario = +)
    pcnt_chan_config_t chanBCfg = {};
    chanBCfg.edge_gpio_num  = PIN_ENC_DT;
    chanBCfg.level_gpio_num = PIN_ENC_CLK;
    pcnt_new_channel(pcntEncoderUnit, &chanBCfg, &pcntChanB);
    pcnt_channel_set_edge_action(pcntChanB,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(pcntChanB,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // Re-asegurar pull-ups internos después de que el driver PCNT enrute la matriz GPIO
    gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_CLK));
    gpio_pullup_en(static_cast<gpio_num_t>(PIN_ENC_DT));

    // accum_count solo acumula más allá de ±32000 si hay watch points en los límites; sin ellos el
    // contador volvía a 0 al desbordar y generaba un salto de miles de muescas.
    pcnt_unit_add_watch_point(pcntEncoderUnit, unitCfg.high_limit);
    pcnt_unit_add_watch_point(pcntEncoderUnit, unitCfg.low_limit);

    pcnt_unit_enable(pcntEncoderUnit);
    pcnt_unit_clear_count(pcntEncoderUnit);
    pcnt_unit_start(pcntEncoderUnit);
    hwPcntActive = true;
    return;
  }
#endif

  // Respaldo por interrupción solo si PCNT no estuvo disponible
  encPrevState = static_cast<uint8_t>((digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoderAB, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  isrEncoderAB, CHANGE);
}

int32_t getEncoderPulseCount() {
#if defined(HAS_ESP32S3_HW_PCNT)
  if (hwPcntActive && pcntEncoderUnit != nullptr) {
    int count = 0;
    if (pcnt_unit_get_count(pcntEncoderUnit, &count) == ESP_OK) {
      return static_cast<int32_t>(count);
    }
  }
#endif
  return encRawPulses;
}
