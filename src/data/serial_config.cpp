#include "serial_config.h"
#include "storage.h"
#include <Arduino.h>
#include <cstring>

#define LINE_BUF_SIZE 200

static char _line[LINE_BUF_SIZE];
static size_t _len = 0;

static void handle_line(char *line) {
    // Trim trailing CR/LF/whitespace
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    if (n == 0) return;

    if (strncmp(line, "TOKEN=", 6) == 0) {
        const char *token = line + 6;
        strlcpy(g_config.airportdb_token, token, sizeof(g_config.airportdb_token));
        storage_save_config(g_config);
        Serial.printf("Saved airportdb.io token (%d chars)\n", (int)strlen(g_config.airportdb_token));
    } else {
        Serial.println("Unknown command. Supported: TOKEN=<airportdb.io token>");
    }
}

void serial_config_poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            _line[_len] = '\0';
            handle_line(_line);
            _len = 0;
        } else if (_len < LINE_BUF_SIZE - 1) {
            _line[_len++] = c;
        }
        // else: silently drop overlong input rather than overflow
    }
}
