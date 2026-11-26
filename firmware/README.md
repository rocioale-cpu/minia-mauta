# Firmware ESP32 (MinIA Mauta)

Firmware para NodeMCU DevKit (ESP32) con micrófono INMP441 y amplificador MAX98357A.
Implementa el contrato v1 ([`docs/contrato-esp32.md`](../docs/contrato-esp32.md)): captura la voz,
la **transmite en streaming** al servidor y reproduce el audio PCM16LE que recibe.

## Requisitos

- Arduino IDE con el core **Arduino-ESP32 3.x** (probado con 3.3.11; usa la API I2S legacy
  `driver/i2s.h`, que compila con una advertencia de deprecación). Placa: *ESP32 Dev Module*.
- Librería **ArduinoJson 6.x** (Library Manager).

## Configuración

Editar al inicio de `minia_mauta/minia_mauta.ino`:

| Constante | Valor |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Red Wi-Fi de **2,4 GHz** |
| `URL_HOST` / `URL_PORT` | Host del servidor en Render (sin `https://`) y 443 |
| `DEVICE_TOKEN` | Igual al `DEVICE_TOKEN` del servicio. **No subir el token real al repositorio.** |

## Pines

| Señal | GPIO |
|---|---|
| I2S BCLK (INMP441 SCK + MAX98357A BCLK) | 14 |
| I2S WS/LRC (compartido) | 15 |
| INMP441 SD (entrada) | 33 |
| MAX98357A DIN (salida) | 22 |
| LED verde / rojo | 18 / 19 |

El INMP441 debe tener `L/R` a GND (canal izquierdo). La salida es mono (canal izquierdo).

## Cómo funciona

1. Al arrancar: Wi-Fi, calibración del ruido y creación/reanudación de sesión (saludo + pregunta).
2. En cada turno abre la conexión TLS **antes** de escuchar y espera voz. Mientras nadie habla solo
   conserva 0,3 s de pre-roll: no envía silencio.
3. Con voz sostenida envía la petición `Transfer-Encoding: chunked` y transmite chunks de 100 ms
   mientras el niño habla. No hay límite de RAM: lo limita el servidor (30 s).
4. Tras 1,2 s de silencio cierra el cuerpo, espera la evaluación (puede tardar varios segundos) y
   reproduce la respuesta en streaming.
5. El avance (pregunta, pista, fin) lo decide el servidor: el firmware lee `estado`, `resultado`,
   `preguntaN` y `fin` del JSON del frame. LED verde = `CORRECTO`; rojo = `INCORRECTO`/`PARCIAL`;
   parpadeo rojo = error de red, `ERROR` de IA o `SIN_CONFIANZA` (se vuelve a escuchar).
6. Errores: 401 detiene el firmware con el LED rojo fijo; 404/400 crean una sesión nueva; una red
   caída parpadea y reintenta.

## Pruebas

El firmware solo se verifica con la placa. Orden sugerido: monitor serie a 115200, comprobar
`crearSesion HTTP 200`, hablar y comprobar `Voz detectada: transmitiendo...` y la línea
`HTTP 200 estado=... resultado=...`.
