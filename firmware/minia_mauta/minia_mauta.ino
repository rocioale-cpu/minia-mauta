#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <math.h>
#include <ArduinoJson.h>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// =========================================================
// 1. CONFIGURACIÓN
// =========================================================
const char* WIFI_SSID     = "TU_NOMBRE_DE_WIFI";      // 2.4 GHz
const char* WIFI_PASSWORD = "TU_PASSWORD_DE_WIFI";

const char* URL_HOST = "minia-mauta-nx10.onrender.com";   // solo el host, sin https://
const int   URL_PORT = 443;
const char* DEVICE_TOKEN = "4c5c9b87-33d2-4cc8-bbb6-5f1eb906d494";
const char* DEVICE_ID = "esp32-01";

// =========================================================
// 2. PINES (idénticos al código original)
// =========================================================
#define I2S_WS   15
#define I2S_SCK  14
#define I2S_SD   33
#define I2S_DOUT 22

#define PIN_LED_VERDE 18
#define PIN_LED_ROJO  19

// =========================================================
// 3. PARÁMETROS TÉCNICOS
// =========================================================
#define I2S_PORT                  I2S_NUM_0
#define SAMPLE_RATE               16000
#define BUFFER_LEN                256
#define CHUNK_MUESTRAS_100MS      1600

#define TIEMPO_SILENCIO_VAD_MS    1200
#define DURACION_MAX_LOCUCION_MS  27000   // el servidor acepta 30 s en total

#define GANANCIA_VOZ              4.0f
#define MINIMO_SONIDO_FUERTE      200.0f
#define VOLUMEN_BOCINA            0.15f
#define FRECUENCIA_PITIDO_HZ      880.0f
#define DURACION_PITIDO_MS        250

int32_t rawBuffer[BUFFER_LEN];
bool i2sActivo = false;

float dc_xPrev = 0.0f, dc_yPrev = 0.0f;
float hp_xPrev = 0.0f, hp_yPrev = 0.0f;
float lp_yPrev = 0.0f;
float pisoRuidoFijo = 40.0f;
float energiaSuavizada = 0.0f;

int pregunta_n = 1;
int aciertos = 0;
int intento = 1;
bool sesionTerminada = false;
String sesionId = "";

// Timeouts. El servidor tarda en evaluar (Gemini) y sintetizar la voz de respuesta.
#define HTTP_TIMEOUT_MS           90000
#define HTTP_CONNECT_TIMEOUT_MS   20000

// Streaming: el audio sale por chunks HTTP de 100 ms mientras el niño habla (sin buffer grande).
#define CHUNK_SAMPLES             1600                      // 100 ms a 16 kHz
#define CHUNK_BYTES               (CHUNK_SAMPLES * 2)
#define DMA_DRENAJE_MS            200                       // espera a que el DMA termine de sonar
#define ESPERA_VOZ_MS             20000                     // sin voz tras este tiempo: se vuelve a escuchar

// Ventana previa al envío: solo se transmite cuando hay voz sostenida; se conserva un pre-roll.
#define PREROLL_MUESTRAS          4800                      // 0,3 s antes de la voz
#define ARRANQUE_MUESTRAS         16000                     // 1 s máx. de ventana (32 KB)
#define BLOQUES_VOZ_PARA_ENVIAR   12                        // ~190 ms de voz para empezar a enviar
int16_t arranque[ARRANQUE_MUESTRAS];

// Bloque ya filtrado (con ganancia) de la última lectura I2S. Se filtra UNA sola vez por muestra.
int16_t bloqueLimpio[BUFFER_LEN];
int bloqueMuestras = 0;

// =========================================================
// 3b. TIPOS (antes de cualquier función: el preprocesador de Arduino genera prototipos arriba)
// =========================================================
#define ERR_RED   (-1)    // sin conexión, timeout o corte de red
#define SIN_VOZ   (0)     // nadie habló: no se envió nada al servidor

// Datos que el servidor devuelve en el JSON del frame de respuesta.
struct RespuestaServidor {
  int httpCode = ERR_RED;
  bool metaOk = false;
  String estado;        // PREGUNTANDO, PISTA, SIN_CONFIANZA, ERROR, FINALIZADA...
  String resultado;     // CORRECTO, INCORRECTO, PARCIAL, SIN_CONFIANZA, ERROR, NINGUNO
  String codigoError;
  String sesionId;
  int preguntaN = 0;
  int totalPreguntas = 0;
  bool fin = false;
};

// Cabeceras HTTP de la respuesta que interesan.
struct RespHttp {
  int  status = ERR_RED;
  long contentLength = -1;
  bool chunked = false;
  String estado;
  String resultado;
};

// Lector de cuerpo: soporta Content-Length, Transfer-Encoding: chunked y lectura hasta cierre.
struct Cuerpo {
  WiFiClient* c = nullptr;
  bool chunked = false;
  long restante = -1;     // bytes por leer del tramo actual; -1 = hasta que cierre la conexión
  bool fin = false;
};


// =========================================================
// 4. I2S HALF-DUPLEX
// =========================================================
void iniciarModoEscucha() {
  if (i2sActivo) { i2s_driver_uninstall(I2S_PORT); i2sActivo = false; }
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 128,
    .use_apll = false
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_start(I2S_PORT);
  i2sActivo = true;
  dc_xPrev = dc_yPrev = hp_xPrev = hp_yPrev = lp_yPrev = 0.0f;
}

void deshabilitarI2S() {
  if (i2sActivo) { i2s_stop(I2S_PORT); i2s_driver_uninstall(I2S_PORT); i2sActivo = false; }
}

void iniciarModoParlante() {
  if (i2sActivo) { i2s_driver_uninstall(I2S_PORT); i2sActivo = false; }
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    // El audio del servidor es PCM16 MONO. Con RIGHT_LEFT cada par de muestras se leía como
    // (izquierda, derecha) y la voz sonaba al doble de velocidad.
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
  i2sActivo = true;
}

// =========================================================
// 5. FILTRADO DE VOZ (idéntico al original)
// =========================================================
float filtrarMuestraVoz(int32_t raw) {
  float x = (float)(raw >> 15);
  float dcOut = x - dc_xPrev + 0.995f * dc_yPrev;
  dc_xPrev = x; dc_yPrev = dcOut;
  const float aHP = 0.9585f;
  float hpOut = aHP * (hp_yPrev + dcOut - hp_xPrev);
  hp_xPrev = dcOut; hp_yPrev = hpOut;
  const float aLP = 0.5718f;
  lp_yPrev = lp_yPrev + aLP * (hpOut - lp_yPrev);
  return lp_yPrev;
}

void analizarBloqueVoz(float &rmsOut, float &freqHzOut, long &rawCrudoOut) {
  size_t bytesIn = 0;
  i2s_read(I2S_PORT, &rawBuffer, sizeof(rawBuffer), &bytesIn, portMAX_DELAY);
  int muestras = bytesIn / sizeof(int32_t);
  bloqueMuestras = 0;
  if (muestras <= 0) { rmsOut = 0; freqHzOut = 0; rawCrudoOut = 0; return; }
  double sumaCuadrados = 0; long sumaCruda = 0; int crucesPorCero = 0; float muestraAnterior = 0.0f;
  for (int i = 0; i < muestras; i++) {
    sumaCruda += abs(rawBuffer[i] >> 16);
    float limpia = filtrarMuestraVoz(rawBuffer[i]) * GANANCIA_VOZ;
    if (limpia > 16000.0f) limpia = 16000.0f + (limpia - 16000.0f) * 0.25f;
    else if (limpia < -16000.0f) limpia = -16000.0f + (limpia + 16000.0f) * 0.25f;
    limpia = constrain(limpia, -32000.0f, 32000.0f);
    bloqueLimpio[i] = (int16_t)limpia;   // se reutiliza al grabar: no se vuelve a filtrar
    if (i > 0) {
      if ((muestraAnterior < 0.0f && limpia >= 0.0f) || (muestraAnterior >= 0.0f && limpia < 0.0f)) crucesPorCero++;
    }
    muestraAnterior = limpia;
    sumaCuadrados += (double)limpia * limpia;
  }
  bloqueMuestras = muestras;
  rawCrudoOut = sumaCruda / muestras;
  rmsOut = sqrt(sumaCuadrados / muestras);
  float duracionSeg = (float)muestras / (float)SAMPLE_RATE;
  freqHzOut = (crucesPorCero / 2.0f) / duracionSeg;
}

void calibrarRuidoInicial() {
  Serial.println("Calibrando ruido...");
  iniciarModoEscucha();
  float rms = 0, freq = 0; long rawCrudo = 0;
  for (int i = 0; i < 50; i++) analizarBloqueVoz(rms, freq, rawCrudo);
  float suma = 0, maximo = 0;
  for (int i = 0; i < 70; i++) {
    analizarBloqueVoz(rms, freq, rawCrudo);
    suma += rms;
    if (rms > maximo) maximo = rms;
  }
  pisoRuidoFijo = max((suma / 70.0f) * 0.6f + maximo * 0.6f, 25.0f);
  energiaSuavizada = pisoRuidoFijo;
  Serial.printf("Piso de ruido: %.1f\n", pisoRuidoFijo);
}

// =========================================================
// 6. HTTP SOBRE TLS (Render) - subida en STREAMING (chunked) y respuesta en streaming
// =========================================================
WiFiClientSecure secureClient;

static uint8_t netBuffer[1024];
static uint8_t txBuffer[16 + CHUNK_BYTES + 2];
static int16_t pcmChunk[CHUNK_SAMPLES];

// UUID v4 aleatorio: el servidor lo usa como X-Turno idempotente.
String nuevoTurnoId() {
  char b[37];
  uint32_t r0 = esp_random(), r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
  snprintf(b, sizeof(b), "%08x-%04x-4%03x-%04x-%04x%08x",
           (unsigned int)r0, (unsigned int)((r1 >> 16) & 0xFFFF), (unsigned int)(r1 & 0x0FFF),
           (unsigned int)(0x8000 | (r2 & 0x3FFF)), (unsigned int)((r2 >> 16) & 0xFFFF), (unsigned int)r3);
  return String(b);
}

bool conectarServidor() {
  secureClient.stop();
  secureClient.setInsecure();   // TODO: fijar el certificado raíz de Render en producción
  secureClient.setTimeout(HTTP_TIMEOUT_MS / 1000);   // WiFiClient::setTimeout recibe segundos
  if (!secureClient.connect(URL_HOST, URL_PORT, HTTP_CONNECT_TIMEOUT_MS)) {
    Serial.println("[ERROR] No se pudo conectar al servidor.");
    return false;
  }
  secureClient.setNoDelay(true);
  return true;
}

bool leerLinea(WiFiClient& client, String& linea, unsigned long timeoutMs) {
  linea = "";
  unsigned long inicio = millis();
  while (millis() - inicio < timeoutMs) {
    if (client.available()) {
      char ch = (char)client.read();
      if (ch == '\n') { linea.trim(); return true; }
      if (linea.length() < 512) linea += ch;
      inicio = millis();
    } else if (!client.connected()) {
      linea.trim();
      return linea.length() > 0;
    } else {
      delay(1);
    }
  }
  return false;
}

bool escribirTodo(WiFiClient& client, const uint8_t* datos, size_t n) {
  size_t enviados = 0;
  unsigned long inicio = millis();
  while (enviados < n) {
    size_t w = client.write(datos + enviados, n - enviados);
    if (w > 0) {
      enviados += w;
      inicio = millis();
    } else {
      if (!client.connected() || millis() - inicio > 5000) return false;
      delay(1);
    }
  }
  return true;
}

// Lee status y cabeceras. Deja el cliente posicionado al inicio del cuerpo.
bool leerCabeceras(WiFiClient& client, RespHttp& r) {
  unsigned long inicio = millis();
  while (!client.available() && client.connected()) {
    if (millis() - inicio > HTTP_TIMEOUT_MS) {
      Serial.println("[ERROR] Timeout esperando respuesta del servidor.");
      return false;
    }
    delay(10);
  }
  String linea;
  if (!leerLinea(client, linea, 5000)) return false;
  int a = linea.indexOf(' ');
  if (a < 0) return false;
  r.status = linea.substring(a + 1, a + 4).toInt();
  if (r.status < 100) return false;

  while (true) {
    if (!leerLinea(client, linea, 5000)) return false;
    if (linea.length() == 0) return true;  // fin de cabeceras
    int sep = linea.indexOf(':');
    if (sep <= 0) continue;
    String k = linea.substring(0, sep);
    String v = linea.substring(sep + 1);
    k.trim(); v.trim();
    if (k.equalsIgnoreCase("Content-Length")) r.contentLength = v.toInt();
    else if (k.equalsIgnoreCase("Transfer-Encoding")) r.chunked = v.equalsIgnoreCase("chunked");
    else if (k.equalsIgnoreCase("X-Estado")) r.estado = v;
    else if (k.equalsIgnoreCase("X-Resultado")) r.resultado = v;
  }
}

Cuerpo abrirCuerpo(WiFiClient& client, const RespHttp& r) {
  Cuerpo b;
  b.c = &client;
  b.chunked = r.chunked;
  b.restante = r.chunked ? 0 : r.contentLength;  // -1 si no hay longitud: hasta cierre
  b.fin = (!r.chunked && r.contentLength == 0);
  return b;
}

// Lee hasta 'max' bytes del cuerpo. Devuelve n>0, 0 si terminó el cuerpo, -1 si hubo error/timeout.
int leerCuerpo(Cuerpo& b, uint8_t* destino, size_t max, unsigned long timeoutMs = 25000) {
  if (b.fin || max == 0) return 0;
  WiFiClient& c = *b.c;
  if (b.chunked && b.restante == 0) {
    String linea;
    if (!leerLinea(c, linea, timeoutMs)) return -1;
    if (linea.length() == 0 && !leerLinea(c, linea, timeoutMs)) return -1;  // CRLF previo
    long tam = strtol(linea.c_str(), NULL, 16);
    if (tam <= 0) { b.fin = true; return 0; }
    b.restante = tam;
  }
  size_t aLeer = max;
  if (b.restante >= 0 && (size_t)b.restante < aLeer) aLeer = (size_t)b.restante;

  unsigned long inicio = millis();
  while (!c.available()) {
    if (!c.connected()) {
      if (b.restante > 0) return -1;   // cuerpo truncado
      b.fin = true;
      return 0;
    }
    if (millis() - inicio > timeoutMs) return -1;
    delay(1);
  }
  int n = c.read(destino, aLeer);
  if (n <= 0) return -1;
  if (b.restante >= 0) {
    b.restante -= n;
    if (b.restante == 0 && !b.chunked) b.fin = true;
  }
  return n;
}

bool leerExactos(Cuerpo& b, uint8_t* destino, size_t n) {
  size_t leidos = 0;
  while (leidos < n) {
    int k = leerCuerpo(b, destino + leidos, n - leidos);
    if (k <= 0) return false;
    leidos += k;
  }
  return true;
}

// Reproduce PCM16LE mono leyendo el resto del cuerpo. Mantiene la paridad de bytes.
bool reproducirCuerpo(Cuerpo& b) {
  iniciarModoParlante();
  size_t total = 0;
  bool carry = false;
  uint8_t carryByte = 0;
  bool ok = true;
  while (true) {
    size_t offset = 0;
    if (carry) { netBuffer[0] = carryByte; offset = 1; carry = false; }
    int n = leerCuerpo(b, netBuffer + offset, sizeof(netBuffer) - offset);
    if (n < 0) { Serial.println("[WARN] Stream de audio truncado."); ok = false; break; }
    if (n == 0) break;
    size_t len = offset + n;
    if (len & 1) { carryByte = netBuffer[len - 1]; carry = true; len--; }
    if (len > 0) {
      size_t escritos = 0;
      i2s_write(I2S_PORT, netBuffer, len, &escritos, portMAX_DELAY);
      total += len;
    }
  }
  // i2s_write vuelve al copiar a la cola DMA: esperar a que se escuche todo antes de silenciar
  if (total > 0) delay(DMA_DRENAJE_MS);
  i2s_zero_dma_buffer(I2S_PORT);
  delay(50);
  deshabilitarI2S();
  Serial.printf("Audio reproducido: %u bytes\n", (unsigned)total);
  return ok;
}

// Procesa una respuesta ya recibida (cabeceras leídas): lee [u32 BE len][JSON][PCM16], rellena resp
// con el JSON y reproduce el audio.
void procesarRespuesta(WiFiClient& client, const RespHttp& r, RespuestaServidor& resp) {
  resp.httpCode = r.status;
  Cuerpo cuerpo = abrirCuerpo(client, r);

  if (r.status != 200) {
    // Cuerpo de error JSON: {"codigo": "...", "mensaje": "..."}
    String err;
    int n;
    while ((n = leerCuerpo(cuerpo, netBuffer, sizeof(netBuffer), 3000)) > 0) {
      for (int i = 0; i < n && err.length() < 400; i++) err += (char)netBuffer[i];
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, err) == DeserializationError::Ok) resp.codigoError = doc["codigo"] | "";
    Serial.printf("[HTTP %d] %s\n", r.status, err.c_str());
    return;
  }

  uint8_t lenBuf[4];
  if (!leerExactos(cuerpo, lenBuf, 4)) { Serial.println("[ERROR] Frame sin longitud."); return; }
  uint32_t jsonLen = ((uint32_t)lenBuf[0] << 24) | ((uint32_t)lenBuf[1] << 16) |
                     ((uint32_t)lenBuf[2] << 8) | (uint32_t)lenBuf[3];
  if (jsonLen == 0 || jsonLen > 65536) { Serial.println("[ERROR] JSON del frame inválido."); return; }

  String jsonMeta;
  jsonMeta.reserve(jsonLen < 3800 ? jsonLen : 3800);
  uint32_t restante = jsonLen;
  while (restante > 0) {
    size_t aLeer = restante < sizeof(netBuffer) ? restante : sizeof(netBuffer);
    if (!leerExactos(cuerpo, netBuffer, aLeer)) { Serial.println("[ERROR] JSON truncado."); return; }
    for (size_t i = 0; i < aLeer && jsonMeta.length() < 3800; i++) jsonMeta += (char)netBuffer[i];
    restante -= aLeer;
  }

  DynamicJsonDocument doc(4096);
  if (jsonLen <= 3800 && deserializeJson(doc, jsonMeta) == DeserializationError::Ok) {
    resp.metaOk = true;
    resp.estado = doc["estado"].as<String>();
    resp.resultado = doc["resultado"].as<String>();
    resp.codigoError = doc["codigoError"] | "";
    resp.sesionId = doc["sesionId"].as<String>();
    resp.preguntaN = doc["preguntaN"] | 0;
    resp.totalPreguntas = doc["totalPreguntas"] | 0;
    resp.fin = doc["fin"] | false;
  } else {
    // Respaldo: usar las cabeceras X-Estado / X-Resultado
    resp.estado = r.estado;
    resp.resultado = r.resultado;
    resp.metaOk = (resp.estado.length() > 0);
  }
  reproducirCuerpo(cuerpo);
}

void escribirCabecerasComunes(const char* metodo, const String& ruta) {
  secureClient.printf("%s %s HTTP/1.1\r\n", metodo, ruta.c_str());
  secureClient.printf("Host: %s\r\n", URL_HOST);
  secureClient.printf("X-Device-Token: %s\r\n", DEVICE_TOKEN);
  secureClient.printf("X-Device-Id: %s\r\n", DEVICE_ID);
  secureClient.print("Connection: close\r\n");
}

// Crea o reanuda la sesión: reproduce saludo + pregunta y devuelve el estado del servidor.
bool crearSesion(RespuestaServidor& resp) {
  resp = RespuestaServidor();
  if (!conectarServidor()) return false;

  String body = "{\"deviceId\":\"" + String(DEVICE_ID) + "\",\"alias\":\"ESP32\"}";
  escribirCabecerasComunes("POST", "/api/v1/device/sesiones");
  secureClient.print("Content-Type: application/json\r\n");
  secureClient.printf("Content-Length: %u\r\n\r\n", (unsigned)body.length());
  secureClient.print(body);

  RespHttp r;
  if (!leerCabeceras(secureClient, r)) { secureClient.stop(); return false; }
  procesarRespuesta(secureClient, r, resp);
  secureClient.stop();
  Serial.printf("crearSesion HTTP %d\n", resp.httpCode);
  return resp.httpCode == 200 && resp.metaOk && resp.sesionId.length() > 0;
}

// Envía un bloque PCM como chunk HTTP (tamaño en hex + datos + CRLF) en un solo write.
bool enviarChunk(const uint8_t* pcm, size_t length) {
  if (length == 0) return true;
  if (!secureClient.connected()) return false;
  int h = snprintf((char*)txBuffer, 16, "%X\r\n", (unsigned)length);
  memcpy(txBuffer + h, pcm, length);
  txBuffer[h + length] = '\r';
  txBuffer[h + length + 1] = '\n';
  return escribirTodo(secureClient, txBuffer, h + length + 2);
}

// =========================================================
// 7. VAD + ENVÍO EN STREAMING
// =========================================================
// 1) Se abre la conexión TLS ANTES de escuchar (el handshake tarda ~1-2 s y no debe perder voz).
// 2) Mientras nadie habla solo se guarda un pre-roll corto: no se envía silencio ni se gasta Gemini.
// 3) Con voz sostenida se envían las cabeceras (Transfer-Encoding: chunked), el pre-roll y después
//    chunks de 100 ms en vivo. No hay límite de RAM: la duración solo la limita el servidor (30 s).
// 4) Tras el silencio final se cierra el cuerpo (chunk 0) y se lee/reproduce la respuesta.
// Devuelve en resp.httpCode: SIN_VOZ (0), ERR_RED (-1) o el código HTTP del servidor.
void capturarYEnviarTurno(const String& sesion, RespuestaServidor& resp) {
  resp = RespuestaServidor();
  if (!conectarServidor()) { resp.httpCode = ERR_RED; return; }

  Serial.println(">>> Escuchando (habla ahora)...");
  iniciarModoEscucha();
  for (int i = 0; i < 4; i++) {                       // estabilizar micrófono y filtros
    float a, b; long c;
    analizarBloqueVoz(a, b, c);
  }

  size_t nArranque = 0;                               // muestras en la ventana previa al envío
  bool enviando = false;
  int bloquesVoz = 0, bloquesSinVoz = 0;
  size_t pendientes = 0;                              // muestras acumuladas en pcmChunk
  unsigned long inicioEscucha = millis(), inicioLocucion = 0, ultimaVoz = 0;

  while (true) {
    float rms = 0, freqHz = 0; long rawCrudo = 0;
    analizarBloqueVoz(rms, freqHz, rawCrudo);
    unsigned long ahora = millis();

    float umbralVoz = (pisoRuidoFijo * 2.0f) + 50.0f;
    bool hayVoz = (rms > umbralVoz) && (freqHz >= 95.0f && freqHz <= 3200.0f);

    if (!enviando) {
      // Ventana deslizante: solo el pre-roll hasta que aparezca voz; luego se amplía.
      size_t limite = (bloquesVoz > 0) ? ARRANQUE_MUESTRAS : PREROLL_MUESTRAS;
      if (nArranque + bloqueMuestras > limite) {
        size_t conservar = (limite > (size_t)bloqueMuestras) ? limite - bloqueMuestras : 0;
        if (nArranque > conservar) {
          memmove(arranque, arranque + (nArranque - conservar), conservar * sizeof(int16_t));
          nArranque = conservar;
        }
      }
      memcpy(arranque + nArranque, bloqueLimpio, bloqueMuestras * sizeof(int16_t));
      nArranque += bloqueMuestras;

      if (hayVoz) { bloquesVoz++; bloquesSinVoz = 0; }
      else if (bloquesVoz > 0 && ++bloquesSinVoz > 25) { bloquesVoz = 0; bloquesSinVoz = 0; }  // ruido aislado

      if (bloquesVoz >= BLOQUES_VOZ_PARA_ENVIAR) {
        // Voz sostenida: abrir la petición chunked y volcar el pre-roll.
        escribirCabecerasComunes("POST", "/api/v1/device/sesiones/" + sesion + "/turno");
        secureClient.printf("X-Turno: %s\r\n", nuevoTurnoId().c_str());
        secureClient.print("Content-Type: audio/L16;rate=16000;channels=1\r\n");
        secureClient.print("Transfer-Encoding: chunked\r\n\r\n");
        for (size_t i = 0; i < nArranque; i += CHUNK_SAMPLES) {
          size_t n = min((size_t)CHUNK_SAMPLES, nArranque - i);
          if (!enviarChunk((const uint8_t*)(arranque + i), n * sizeof(int16_t))) {
            Serial.println("[ERROR] Falló el envío de audio.");
            deshabilitarI2S(); secureClient.stop(); resp.httpCode = ERR_RED; return;
          }
        }
        enviando = true;
        inicioLocucion = ahora;
        ultimaVoz = ahora;
        nArranque = 0;
        Serial.println(">>> Voz detectada: transmitiendo...");
      } else if (ahora - inicioEscucha >= ESPERA_VOZ_MS) {
        deshabilitarI2S();
        secureClient.stop();                           // nunca se envió nada
        resp.httpCode = SIN_VOZ;
        return;
      }
    } else {
      for (int i = 0; i < bloqueMuestras; i++) {
        pcmChunk[pendientes++] = bloqueLimpio[i];
        if (pendientes == CHUNK_SAMPLES) {
          if (!enviarChunk((const uint8_t*)pcmChunk, CHUNK_BYTES)) {
            Serial.println("[ERROR] Falló el envío de audio.");
            deshabilitarI2S(); secureClient.stop(); resp.httpCode = ERR_RED; return;
          }
          pendientes = 0;
        }
      }
      if (hayVoz) ultimaVoz = ahora;
      if ((ahora - ultimaVoz >= TIEMPO_SILENCIO_VAD_MS) || (ahora - inicioLocucion >= DURACION_MAX_LOCUCION_MS)) {
        break;
      }
    }
  }

  deshabilitarI2S();
  if (pendientes > 0 && !enviarChunk((const uint8_t*)pcmChunk, pendientes * sizeof(int16_t))) {
    secureClient.stop(); resp.httpCode = ERR_RED; return;
  }
  if (!escribirTodo(secureClient, (const uint8_t*)"0\r\n\r\n", 5)) {   // fin del cuerpo chunked
    secureClient.stop(); resp.httpCode = ERR_RED; return;
  }
  Serial.println(">>> Fin de locución. Esperando evaluación...");

  RespHttp r;
  if (!leerCabeceras(secureClient, r)) {
    secureClient.stop();
    resp.httpCode = ERR_RED;
    return;
  }
  procesarRespuesta(secureClient, r, resp);
  secureClient.stop();
}

// =========================================================
// 8. SETUP / LOOP
// =========================================================
void parpadearRojo(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(PIN_LED_ROJO, HIGH); delay(150);
    digitalWrite(PIN_LED_ROJO, LOW);  delay(150);
  }
}

// Intenta crear la sesión; si falla, reintenta con espera. Devuelve false solo si se agotan los intentos.
bool iniciarSesionConReintentos(int maxIntentos) {
  for (int i = 1; i <= maxIntentos; i++) {
    RespuestaServidor resp;
    if (crearSesion(resp)) {
      sesionId = resp.sesionId;
      pregunta_n = resp.preguntaN > 0 ? resp.preguntaN : 1;
      Serial.println("Sesion: " + sesionId);
      return true;
    }
    if (resp.httpCode == 401) {              // token inválido: reintentar no sirve
      Serial.println("TOKEN_INVALIDO: revisa DEVICE_TOKEN");
      return false;
    }
    Serial.printf("Error creando sesion (intento %d/%d)\n", i, maxIntentos);
    parpadearRojo(3);
    delay(3000);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_ROJO, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  calibrarRuidoInicial();

  sesionTerminada = false;
  aciertos = 0;
  if (!iniciarSesionConReintentos(5)) {
    Serial.println("No se pudo crear la sesion; reinicia la placa.");
    digitalWrite(PIN_LED_ROJO, HIGH);   // LED rojo fijo
    sesionTerminada = true;
  }
}

void loop() {
  if (sesionTerminada) { delay(1000); return; }

  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_ROJO, LOW);
  Serial.printf("Pregunta %d\n", pregunta_n);

  RespuestaServidor resp;
  capturarYEnviarTurno(sesionId, resp);
  Serial.printf("HTTP %d estado=%s resultado=%s cod=%s\n", resp.httpCode,
                resp.estado.c_str(), resp.resultado.c_str(), resp.codigoError.c_str());

  if (resp.httpCode == SIN_VOZ) {                           // nadie habló: volver a escuchar
    Serial.println("Sin voz detectada, reintentando...");
    delay(300);
    return;
  }
  // Sin respuesta útil: nunca se interpreta como una evaluación; se vuelve a escuchar.
  if (resp.httpCode == 401) {
    Serial.println("TOKEN_INVALIDO: revisa DEVICE_TOKEN");
    digitalWrite(PIN_LED_ROJO, HIGH);
    sesionTerminada = true;
    return;
  }
  if (resp.httpCode == 404 || resp.httpCode == 400) {       // sesión inexistente o ya finalizada
    Serial.println("Sesion no valida, creando una nueva...");
    if (!iniciarSesionConReintentos(3)) sesionTerminada = true;
    return;
  }
  if (resp.httpCode != 200 || !resp.metaOk) {
    parpadearRojo(3);                                        // red caída o respuesta ilegible
    delay(1000);
    return;
  }

  if (resp.fin || resp.estado == "FINALIZADA") {
    sesionTerminada = true;
    digitalWrite(PIN_LED_VERDE, resp.resultado == "CORRECTO" ? HIGH : LOW);
    Serial.println("Sesion finalizada.");
    return;
  }

  if (resp.resultado == "CORRECTO") {
    aciertos++;
    digitalWrite(PIN_LED_VERDE, HIGH);
    digitalWrite(PIN_LED_ROJO, LOW);
  } else if (resp.resultado == "INCORRECTO" || resp.resultado == "PARCIAL") {
    digitalWrite(PIN_LED_VERDE, LOW);
    digitalWrite(PIN_LED_ROJO, HIGH);
  } else {
    // ERROR (IA no disponible) o SIN_CONFIANZA: no cuenta como respuesta; el servidor ya pidió repetir.
    parpadearRojo(2);
  }

  if (resp.preguntaN > 0) pregunta_n = resp.preguntaN;      // el servidor manda el avance
  delay(200);
}
