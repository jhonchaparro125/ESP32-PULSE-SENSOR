#include <Arduino.h>
#include <esp_arduino_version.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <PulseSensorPlayground.h>

// ================= CONEXIONES =================
#define TFT_CS     5
#define TFT_DC     2
#define TFT_RST    4
#define TFT_SCLK  18
#define TFT_MOSI  23

const int PULSE_PIN = 34;
const int BUZZER_PIN = 25;

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
PulseSensorPlayground pulseSensor;

// ================= BUZZER Y CORAZON =================
// false: piezo pasivo; true: modulo activo, disparado en HIGH.
const bool BUZZER_ACTIVO = false;
const int FRECUENCIA_BUZZER = 2000;
const int CANAL_BUZZER = 6;  // Para Arduino-ESP32 2.x.
const unsigned long DURACION_PITIDO = 60;
const unsigned long DURACION_CORAZON = 140;

bool buzzerSonando = false;
bool corazonEncendido = false;
unsigned long inicioAviso = 0;

// ================= AJUSTES =================
const int UMBRAL_INICIAL = 550;
int umbralConfigurado = UMBRAL_INICIAL;

const int AMPLITUD_MIN = 20;
const int AMPLITUD_MAX = 600;
const int LIMITE_BAJO = 5;
const int LIMITE_ALTO = 1018;

const unsigned long PERIODO_GRAFICA = 20;
const unsigned long PERIODO_TEXTO = 250;
const unsigned long PERIODO_SERIAL = 20;
const unsigned long VENTANA_CALIDAD = 1000;
const unsigned long SIN_LATIDOS_MS = 3000;
const unsigned long PERIODO_RECALIBRACION = 3000;
const float GANANCIA_GRAFICA = 1.0f;

// ================= GRAFICA =================
const int GRAFICA_ARRIBA = 100;
const int GRAFICA_ABAJO = 219;
const int GRAFICA_CENTRO = 160;
const int PASO_CUADRICULA = 20;

const uint16_t COLOR_FONDO = ST77XX_BLACK;
const uint16_t COLOR_CUADRICULA = 0x2104;
const uint16_t COLOR_CENTRO = 0x4208;

// ================= VARIABLES =================
int bpm = 0;
int bpmAnterior = -1;
int estadoAnterior = -1;
int ultimaMuestra = 0;
bool calidadAceptable = false;
bool filtroIniciado = false;
bool huboLatido = false;
float lineaBase = 0;
float curva = 0;
int xGrafica = 0;
int yAnterior = GRAFICA_CENTRO;
int minimoVentana = 1023;
int maximoVentana = 0;
int muestrasVentana = 0;
int muestrasSaturadas = 0;
int minimoRegistrado = 0;
int maximoRegistrado = 0;
int amplitudRegistrada = 0;

unsigned long totalLatidos = 0;
unsigned long ultimoLatido = 0;
unsigned long ultimaGrafica = 0;
unsigned long ultimoTexto = 0;
unsigned long ultimoSerial = 0;
unsigned long inicioVentana = 0;
unsigned long ultimaRecalibracion = 0;

// ================= SONIDO SIN PAUSAS =================
bool configurarBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  if (BUZZER_ACTIVO) return true;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(BUZZER_PIN, FRECUENCIA_BUZZER, 8)) {
    return false;
  }
  ledcWrite(BUZZER_PIN, 0);
#else
  if (ledcSetup(CANAL_BUZZER, FRECUENCIA_BUZZER, 8) == 0) {
    return false;
  }
  ledcAttachPin(BUZZER_PIN, CANAL_BUZZER);
  ledcWrite(CANAL_BUZZER, 0);
#endif
  return true;
}

void activarSonido(bool activar) {
  if (BUZZER_ACTIVO) {
    digitalWrite(BUZZER_PIN, activar ? HIGH : LOW);
    return;
  }

  // PWM por hardware: no usa tone() ni esperas.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(BUZZER_PIN, activar ? 128 : 0);
#else
  ledcWrite(CANAL_BUZZER, activar ? 128 : 0);
#endif
}

void dibujarCorazon(bool encendido) {
  // Area reservada: x=228..275, y=28..75.
  tft.fillRect(228, 28, 48, 48, COLOR_FONDO);
  const int cx = 252;
  const int cy = 48;
  int r = encendido ? 8 : 6;
  uint16_t color = encendido ? ST77XX_RED : 0x6000;

  tft.fillCircle(cx - r, cy - r, r, color);
  tft.fillCircle(cx + r, cy - r, r, color);
  tft.fillTriangle(cx - 2 * r, cy - r,
                   cx + 2 * r, cy - r,
                   cx, cy + 2 * r, color);
}

void avisarLatido(unsigned long ahora) {
  inicioAviso = ahora;
  activarSonido(true);
  buzzerSonando = true;
  dibujarCorazon(true);
  corazonEncendido = true;
}

void actualizarAvisos(unsigned long ahora) {
  if (buzzerSonando &&
      (!calidadAceptable || ahora - inicioAviso >= DURACION_PITIDO)) {
    activarSonido(false);
    buzzerSonando = false;
  }
  if (corazonEncendido &&
      (!calidadAceptable || ahora - inicioAviso >= DURACION_CORAZON)) {
    dibujarCorazon(false);
    corazonEncendido = false;
  }
}

// ================= CALIDAD Y UMBRAL =================
void evaluarCalidad(int muestra, unsigned long ahora) {
  minimoVentana = min(minimoVentana, muestra);
  maximoVentana = max(maximoVentana, muestra);
  muestrasVentana++;

  if (muestra <= LIMITE_BAJO || muestra >= LIMITE_ALTO) {
    muestrasSaturadas++;
  }
  if (ahora - inicioVentana < VENTANA_CALIDAD) return;

  minimoRegistrado = minimoVentana;
  maximoRegistrado = maximoVentana;
  amplitudRegistrada = maximoVentana - minimoVentana;

  calidadAceptable = muestrasVentana >= 25 &&
      amplitudRegistrada >= AMPLITUD_MIN &&
      amplitudRegistrada <= AMPLITUD_MAX &&
      muestrasSaturadas * 10 < muestrasVentana;

  bool sinEventosRecientes =
      !huboLatido || ahora - ultimoLatido > SIN_LATIDOS_MS;

  if (calidadAceptable && sinEventosRecientes &&
      ahora - ultimaRecalibracion >= PERIODO_RECALIBRACION) {
    int nuevoUmbral = minimoRegistrado + amplitudRegistrada / 2;
    nuevoUmbral = constrain(nuevoUmbral, 20, 1000);
    pulseSensor.setThreshold(nuevoUmbral);
    umbralConfigurado = nuevoUmbral;
    ultimaRecalibracion = ahora;
  }

  minimoVentana = 1023;
  maximoVentana = 0;
  muestrasVentana = 0;
  muestrasSaturadas = 0;
  inicioVentana = ahora;
}

// ================= CUADRICULA =================
void restaurarColumna(int x) {
  int alto = GRAFICA_ABAJO - GRAFICA_ARRIBA + 1;
  uint16_t color = (x % PASO_CUADRICULA == 0)
      ? COLOR_CUADRICULA : COLOR_FONDO;
  tft.drawFastVLine(x, GRAFICA_ARRIBA, alto, color);
  for (int y = GRAFICA_ARRIBA; y <= GRAFICA_ABAJO;
       y += PASO_CUADRICULA) {
    tft.drawPixel(x, y, COLOR_CUADRICULA);
  }
  tft.drawPixel(x, GRAFICA_CENTRO, COLOR_CENTRO);
}

void dibujarCuadricula() {
  for (int x = 0; x < tft.width(); x++) restaurarColumna(x);
}

// ================= INTERFAZ TFT =================
void dibujarInterfaz() {
  tft.fillScreen(COLOR_FONDO);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(5, 5);
  tft.print("PULSE SENSOR");
  tft.drawFastHLine(0, 94, tft.width(), ST77XX_WHITE);
  dibujarCuadricula();
  dibujarCorazon(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 229);
  tft.print("Dedo quieto - presion suave");
}

void actualizarTexto() {
  int estado = !calidadAceptable ? 0 : (bpm > 0 ? 2 : 1);

  if (bpm != bpmAnterior) {
    // Reserva el extremo derecho para el corazon.
    tft.fillRect(0, 28, 220, 48, COLOR_FONDO);
    tft.setTextSize(6);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(12, 29);
    if (bpm > 0) tft.print(bpm);
    else tft.print("--");
    tft.setTextSize(3);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(155, 45);
    tft.print("BPM");
    bpmAnterior = bpm;
  }

  if (estado != estadoAnterior) {
    tft.fillRect(0, 78, tft.width(), 12, COLOR_FONDO);
    tft.setTextSize(1);
    tft.setCursor(5, 80);
    if (estado == 0) {
      tft.setTextColor(ST77XX_YELLOW);
      tft.print("Revise contacto / luz");
    } else if (estado == 1) {
      tft.setTextColor(ST77XX_WHITE);
      tft.print("Buscando pulso / ajustando umbral");
    } else {
      tft.setTextColor(ST77XX_GREEN);
      tft.print("BPM de biblioteca - verificar");
    }
    estadoAnterior = estado;
  }
}

void dibujarCurva(float valor) {
  int y = GRAFICA_CENTRO - (int)(valor * GANANCIA_GRAFICA);
  y = constrain(y, GRAFICA_ARRIBA, GRAFICA_ABAJO);
  restaurarColumna(xGrafica);
  uint16_t color = calidadAceptable ? ST77XX_GREEN : ST77XX_YELLOW;
  if (xGrafica > 0) {
    tft.drawLine(xGrafica - 1, yAnterior, xGrafica, y, color);
  } else {
    tft.drawPixel(xGrafica, y, color);
  }
  yAnterior = y;
  xGrafica++;
  if (xGrafica >= tft.width()) xGrafica = 0;
}

void detenerConError(const char* linea1, const char* linea2) {
  tft.fillScreen(COLOR_FONDO);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(5, 60);
  tft.print(linea1);
  tft.setCursor(5, 90);
  tft.print(linea2);
  Serial.println(linea1);
  Serial.println(linea2);
  while (true) delay(1000);
}

// ================= SERIAL PLOTTER =================
void imprimirDiagnostico() {
  Serial.print("PPG:");
  Serial.println(curva, 2);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 280);
  tft.setRotation(1);
  dibujarInterfaz();
  actualizarTexto();

  if (!configurarBuzzer()) {
    detenerConError("Error en buzzer", "Revise PWM / pin");
  }

  analogReadResolution(10);
  pinMode(PULSE_PIN, INPUT);
  pulseSensor.analogInput(PULSE_PIN);
  pulseSensor.setThreshold(UMBRAL_INICIAL);
  if (!pulseSensor.begin()) {
    detenerConError("Error al iniciar", "Pulse Sensor");
  }
  if (!pulseSensor.UsingHardwareTimer) {
    detenerConError("Sin temporizador", "Revise biblioteca");
  }
  inicioVentana = millis();
  ultimaRecalibracion = inicioVentana;
}

// ================= LOOP =================
void loop() {
  pulseSensor.sawNewSample();
  unsigned long ahora = millis();

  if (pulseSensor.sawStartOfBeat()) {
    ultimoLatido = ahora;
    huboLatido = true;
    totalLatidos++;
    // Sonido y corazon usan el mismo evento del detector.
    if (calidadAceptable) avisarLatido(ahora);
  }

  if (ahora - ultimaGrafica >= PERIODO_GRAFICA) {
    ultimaGrafica = ahora;
    ultimaMuestra = pulseSensor.getLatestSample();
    evaluarCalidad(ultimaMuestra, ahora);
    if (!filtroIniciado) {
      lineaBase = ultimaMuestra;
      curva = 0;
      filtroIniciado = true;
    }
    lineaBase += 0.025f * (ultimaMuestra - lineaBase);
    float centrada = ultimaMuestra - lineaBase;
    curva += 0.35f * (centrada - curva);
    dibujarCurva(curva);
  }

  bool eventoReciente = huboLatido &&
      ahora - ultimoLatido <= SIN_LATIDOS_MS;
  if (calidadAceptable && eventoReciente) {
    bpm = pulseSensor.getBeatsPerMinute();
  } else {
    bpm = 0;
  }

  actualizarAvisos(ahora);

  if (ahora - ultimoTexto >= PERIODO_TEXTO) {
    ultimoTexto = ahora;
    actualizarTexto();
  }
  if (ahora - ultimoSerial >= PERIODO_SERIAL) {
    ultimoSerial = ahora;
    imprimirDiagnostico();
  }
}
