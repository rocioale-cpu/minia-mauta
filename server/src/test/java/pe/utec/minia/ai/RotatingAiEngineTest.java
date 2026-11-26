package pe.utec.minia.ai;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import org.mockito.ArgumentCaptor;
import pe.utec.minia.api.error.ApiException;
import pe.utec.minia.config.GeminiKeyPool;
import pe.utec.minia.config.MiniaProperties;

import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.net.http.HttpTimeoutException;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

class RotatingAiEngineTest {

    private static final String GEMINI_RESPONSE = """
            {"candidates":[{"content":{"parts":[{"text":"{\\"resultado\\":\\"CORRECTO\\",\\"confianza\\":0.9,\\"transcripcion\\":\\"sí\\",\\"pista\\":\\"\\",\\"feedback\\":\\"¡Bien!\\"}"}]}}]}
            """;

    @Test
    void reportsTimeoutTypeAndRetriesOnlyOnce() throws Exception {
        HttpClient httpClient = mock(HttpClient.class);
        when(httpClient.send(any(HttpRequest.class), any(HttpResponse.BodyHandler.class)))
                .thenThrow(new HttpTimeoutException("request timed out"));

        ApiException exception = assertThrows(ApiException.class,
                () -> engine(httpClient, List.of("key-a")).evaluar(request()));

        assertEquals("GEMINI_CONEXION", exception.getCodigo());
        assertTrue(exception.getMessage().contains("HttpTimeoutException"));
        verify(httpClient, times(2)).send(any(HttpRequest.class), any(HttpResponse.BodyHandler.class));
    }

    @Test
    void triesAnotherApiKeyAfterAuthenticationFailureWithoutRepeatingTtsCombinations() throws Exception {
        HttpClient httpClient = mock(HttpClient.class);
        HttpResponse<String> forbidden = response(403, "");
        HttpResponse<String> success = response(200, GEMINI_RESPONSE);
        AtomicInteger calls = new AtomicInteger();
        when(httpClient.send(any(HttpRequest.class), any(HttpResponse.BodyHandler.class)))
                .thenAnswer(invocation -> calls.getAndIncrement() == 0 ? forbidden : success);

        Evaluacion evaluation = engine(httpClient, List.of("key-a", "key-b")).evaluar(request());

        assertEquals("CORRECTO", evaluation.resultado().name());
        assertEquals(2, calls.get());

        ArgumentCaptor<HttpRequest> requests = ArgumentCaptor.forClass(HttpRequest.class);
        verify(httpClient, times(2)).send(requests.capture(), any(HttpResponse.BodyHandler.class));
        assertEquals("key-a", requests.getAllValues().get(0).headers()
                .firstValue("x-goog-api-key").orElseThrow());
        assertEquals("key-b", requests.getAllValues().get(1).headers()
                .firstValue("x-goog-api-key").orElseThrow());
    }

    private RotatingAiEngine engine(HttpClient httpClient, List<String> apiKeys) {
        MiniaProperties properties = new MiniaProperties(
                new MiniaProperties.Ai(
                        new MiniaProperties.Ai.GeminiAi(apiKeys, "gemini-2.5-flash", 9)),
                null,
                new MiniaProperties.Audio(16_000, 1, 30, 100, 300),
                null,
                null
        );
        GeminiKeyPool keyPool = new GeminiKeyPool(
                apiKeys, List.of("tts-model-a", "tts-model-b"), "gemini-2.5-flash");
        return new RotatingAiEngine(properties, new ObjectMapper(), httpClient, keyPool);
    }

    private EvaluacionRequest request() {
        return new EvaluacionRequest(
                new byte[]{0, 0},
                16_000,
                "Texto",
                "Pregunta",
                "Respuesta",
                List.of(),
                1,
                1,
                1
        );
    }

    @SuppressWarnings("unchecked")
    private HttpResponse<String> response(int status, String body) {
        HttpResponse<String> response = mock(HttpResponse.class);
        when(response.statusCode()).thenReturn(status);
        when(response.body()).thenReturn(body);
        return response;
    }
}
