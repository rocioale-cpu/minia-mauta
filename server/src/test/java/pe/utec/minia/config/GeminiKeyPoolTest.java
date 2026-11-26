package pe.utec.minia.config;

import org.junit.jupiter.api.Test;

import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;

class GeminiKeyPoolTest {

    @Test
    void trimsAndDeduplicatesKeysAndRotatesTheFirstKey() {
        GeminiKeyPool pool = new GeminiKeyPool(
                List.of(" key-a ", "key-b", "key-a"),
                List.of(" tts-a ", "tts-a"),
                " gemini-model "
        );

        assertEquals(List.of("key-a", "key-b"), pool.apiKeys());
        assertEquals(2, pool.totalCombinaciones());
        assertEquals("gemini-model", pool.llmModel());
        assertEquals(List.of("key-a", "key-b"), pool.siguienteOrdenApiKeys());
        assertEquals(List.of("key-b", "key-a"), pool.siguienteOrdenApiKeys());
    }
}
