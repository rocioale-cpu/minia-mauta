package pe.utec.minia.ai;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import pe.utec.minia.api.error.ApiException;
import pe.utec.minia.audio.AudioUtils;
import pe.utec.minia.config.GeminiKeyPool;
import pe.utec.minia.config.MiniaProperties;
import pe.utec.minia.domain.Resultado;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import java.util.Base64;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class RotatingAiEngine implements AiEngine {

    private static final Logger log = LoggerFactory.getLogger(RotatingAiEngine.class);

    private final MiniaProperties properties;
    private final ObjectMapper mapper;
    private final HttpClient httpClient;
    private final GeminiKeyPool keyPool;

    public RotatingAiEngine(MiniaProperties properties, ObjectMapper mapper, HttpClient httpClient, GeminiKeyPool keyPool) {
        this.properties = properties;
        this.mapper = mapper;
        this.httpClient = httpClient;
        this.keyPool = keyPool;
    }

    @Override
    public Evaluacion evaluar(EvaluacionRequest request) {
        int audioBytes = request.audioPcm() == null ? 0 : request.audioPcm().length;
        double duracionSeg = audioBytes / (2.0 * Math.max(1, request.rate()));
        log.info(">>> [AUDIO RECIBIDO EN SERVIDOR] {} bytes ({} s) | rate={} Hz | intento={}",
                audioBytes, String.format(Locale.ROOT, "%.2f", duracionSeg), request.rate(), request.intento());

        int intentos = keyPool.totalCombinaciones();
        if (intentos == 0) {
            throw ApiException.serviceUnavailable("AI_SIN_KEYS", "No hay API keys configuradas para IA.");
        }

        Set<String> modelosCandidatos = new LinkedHashSet<>();
        String configurado = keyPool.llmModel();
        if (configurado != null && !configurado.isBlank() && !configurado.contains("3.8")) {
            modelosCandidatos.add(configurado.trim());
        }
        modelosCandidatos.add("gemini-2.5-flash");
        modelosCandidatos.add("gemini-2.5-flash-lite");
        modelosCandidatos.add("gemini-2.0-flash");

        String ultimoError = "No se pudo conectar con Gemini.";

        for (int i = 0; i < Math.max(1, intentos); i++) {
            GeminiKeyPool.Combinacion combo = keyPool.siguienteDisponible();
            if (combo == null) {
                break;
            }
            String apiKey = combo.apiKey();
            if (apiKey == null || apiKey.isBlank()) {
                continue;
            }

            try {
                ObjectNode body = mapper.createObjectNode();
                body.set("systemInstruction", systemInstruction());
                body.set("contents", contents(request));

                ObjectNode generationConfig = body.putObject("generationConfig");
                generationConfig.put("temperature", 0.2);
                generationConfig.put("responseMimeType", "application/json");
                generationConfig.set("responseSchema", responseSchema());

                String payload = mapper.writeValueAsString(body);
                boolean hubo429 = false;

                for (String model : modelosCandidatos) {
                    String url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
                    try {
                        HttpResponse<String> response = enviar(url, apiKey, payload);
                        if (response.statusCode() == 200) {
                            String text = extractText(mapper.readTree(response.body()));
                            log.info(">>> [EVALUACION GEMINI] {}", text);
                            return parseEvaluacion(text);
                        }
                        if (response.statusCode() == 429) {
                            hubo429 = true;
                        }
                        ultimoError = "HTTP " + response.statusCode() + " en " + model;
                        log.warn("Eval fallo en {} -> HTTP {}: {}", model, response.statusCode(), response.body());
                    } catch (IOException ioEx) {
                        ultimoError = ioEx.getClass().getSimpleName() + ": " + ioEx.getMessage();
                        log.warn("Error de red/timeout en {} -> {}", model, ultimoError);
                    }
                }

                if (hubo429) {
                    keyPool.marcarAgotada(combo.keyIndex(), combo.modelIndex());
                }
            } catch (InterruptedException exception) {
                Thread.currentThread().interrupt();
                throw ApiException.serviceUnavailable("GEMINI_INTERRUMPIDO", "La evaluación fue interrumpida.");
            } catch (IOException exception) {
                throw ApiException.serviceUnavailable("GEMINI_ERROR", "Error preparando solicitud para Gemini.");
            }
        }

        throw ApiException.serviceUnavailable("GEMINI_ERROR", ultimoError);
    }

    private HttpResponse<String> enviar(String url, String apiKey, String payload)
            throws IOException, InterruptedException {
        int timeoutSeg = Math.min(15, Math.max(8, properties.ai().gemini().timeoutSeconds()));
        HttpRequest request = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofSeconds(timeoutSeg))
                .header("Content-Type", "application/json")
                .header("x-goog-api-key", apiKey)
                .POST(HttpRequest.BodyPublishers.ofString(payload))
                .build();
        return httpClient.send(request, HttpResponse.BodyHandlers.ofString());
    }

    private ObjectNode systemInstruction() {
        ObjectNode system = mapper.createObjectNode();
        ArrayNode parts = system.putArray("parts");
        parts.addObject().put("text", """
                Eres un evaluador de comprensión lectora para niños de primaria en Perú.
                Evalúa si la respuesta oral del niño es correcta respecto de la respuesta esperada.
                Sé tolerante con sinónimos, errores menores de pronunciación y respuestas parciales.
                Si el audio es inaudible o no contiene una respuesta clara, usa SIN_CONFIANZA.
                Devuelve SOLO el JSON solicitado, sin markdown.
                El feedback debe ser breve, amable y en español, de máximo dos frases.
                """);
        return system;
    }

    private ArrayNode contents(EvaluacionRequest request) {
        List<String> palabrasClave = request.palabrasClave() == null ? List.of() : request.palabrasClave();
        String prompt = """
                TEXTO:
                %s

                PREGUNTA:
                %s

                RESPUESTA ESPERADA:
                %s

                PALABRAS CLAVE:
                %s

                LÍNEA DE PISTA: %d
                INTENTO: %d
                """.formatted(
                request.texto(),
                request.pregunta(),
                request.respuestaEsperada(),
                String.join(", ", palabrasClave),
                request.lineaPista(),
                request.intento()
        );

        ArrayNode contents = mapper.createArrayNode();
        ObjectNode content = contents.addObject();
        content.put("role", "user");
        ArrayNode parts = content.putArray("parts");
        parts.addObject().put("text", prompt);

        byte[] wav = AudioUtils.wavFromPcm(request.audioPcm(), request.rate(), 1);
        ObjectNode inline = parts.addObject().putObject("inlineData");
        inline.put("mimeType", "audio/wav");
        inline.put("data", Base64.getEncoder().encodeToString(wav));
        return contents;
    }

    private ObjectNode responseSchema() {
        ObjectNode schema = mapper.createObjectNode();
        schema.put("type", "OBJECT");
        ObjectNode propertiesNode = schema.putObject("properties");

        ObjectNode resultado = propertiesNode.putObject("resultado");
        resultado.put("type", "STRING");
        resultado.putArray("enum").add("CORRECTO").add("INCORRECTO").add("SIN_CONFIANZA");

        propertiesNode.putObject("confianza").put("type", "NUMBER");
        propertiesNode.putObject("transcripcion").put("type", "STRING");
        propertiesNode.putObject("pista").put("type", "STRING");
        propertiesNode.putObject("feedback").put("type", "STRING");

        schema.putArray("required")
                .add("resultado")
                .add("confianza")
                .add("transcripcion")
                .add("pista")
                .add("feedback");
        return schema;
    }

    private String extractText(JsonNode root) {
        JsonNode parts = root.path("candidates").path(0).path("content").path("parts");
        StringBuilder text = new StringBuilder();
        for (JsonNode part : parts) {
            if (part.path("thought").asBoolean(false)) {
                continue;
            }
            if (part.hasNonNull("text")) {
                text.append(part.path("text").asText());
            }
        }
        if (text.isEmpty()) {
            throw ApiException.serviceUnavailable("GEMINI_SIN_TEXTO", "Gemini no devolvió una evaluación.");
        }
        return text.toString();
    }

    private Evaluacion parseEvaluacion(String text) throws IOException {
        String clean = text.trim();
        if (clean.startsWith("```")) {
            int firstLine = clean.indexOf('\n');
            int lastFence = clean.lastIndexOf("```");
            if (firstLine >= 0 && lastFence > firstLine) {
                clean = clean.substring(firstLine + 1, lastFence).trim();
            }
        }
        JsonNode node = mapper.readTree(clean);
        Resultado resultado;
        try {
            resultado = Resultado.valueOf(node.path("resultado").asText("SIN_CONFIANZA").trim().toUpperCase());
        } catch (IllegalArgumentException exception) {
            resultado = Resultado.SIN_CONFIANZA;
        }
        double confianza = Math.max(0, Math.min(1, node.path("confianza").asDouble(0.5)));
        String transcripcion = node.path("transcripcion").asText("");
        String pista = node.path("pista").asText("");
        String feedback = node.path("feedback").asText("");
        if (feedback.isBlank()) {
            feedback = resultado == Resultado.CORRECTO
                    ? "¡Muy bien! Esa es la respuesta correcta."
                    : "Vamos a revisar el texto y volver a intentarlo.";
        }
        return new Evaluacion(resultado, confianza, transcripcion, pista, feedback);
    }

    @Override
    public String nombre() {
        return "gemini-rotating:" + keyPool.totalCombinaciones() + " combos";
    }
}
