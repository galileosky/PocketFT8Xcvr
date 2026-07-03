#include <Arduino.h>
#include <MTP_Teensy.h>
#include <SD.h>

static constexpr uint8_t kSdCardPin = BUILTIN_SDCARD;
elapsedMillis statusTimer;

#if defined(SERIAL_DIAGNOSTICS) && (SERIAL_DIAGNOSTICS == 1)
static constexpr bool kSerialDiagnosticsEnabled = true;
#else
static constexpr bool kSerialDiagnosticsEnabled = false;
#endif

#if defined(SERIAL_DIAG_HEARTBEAT_MS)
static constexpr uint32_t kSerialDiagHeartbeatMs = SERIAL_DIAG_HEARTBEAT_MS;
#else
static constexpr uint32_t kSerialDiagHeartbeatMs = 5000;
#endif

static void printRootSummary(const uint8_t maxEntries) {
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("[DIAG] root open failed");
        return;
    }

    uint8_t count = 0;
    while (count < maxEntries) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }

        Serial.print("[DIAG] ");
        Serial.print(entry.name());
        if (entry.isDirectory()) {
            Serial.println("/");
        } else {
            Serial.print(" (");
            Serial.print(entry.size());
            Serial.println(" B)");
        }

        entry.close();
        count++;
    }

    root.close();
}

void setup() {
    delay(1000);
    Serial.begin(115200);
    while (!Serial && millis() < 4000) {
    }

    if (kSerialDiagnosticsEnabled) {
        Serial.println("[DIAG] booting Teensy 4.1 SD MTP bridge");
    }

    if (!SD.begin(kSdCardPin)) {
        if (kSerialDiagnosticsEnabled) {
            Serial.println("[DIAG] SD init failed");
            Serial.println("[DIAG] MTP disabled until SD init succeeds");
        }
        return;
    }

    if (kSerialDiagnosticsEnabled) {
        const uint64_t cardBytes = SD.totalSize();
        const uint64_t usedBytes = SD.usedSize();

        Serial.print("[DIAG] SD init ok, total=");
        Serial.print(static_cast<unsigned long long>(cardBytes / (1024ULL * 1024ULL)));
        Serial.print(" MiB, used=");
        Serial.print(static_cast<unsigned long long>(usedBytes / (1024ULL * 1024ULL)));
        Serial.println(" MiB");
        printRootSummary(5);
    }

    MTP.begin();
    MTP.addFilesystem(SD, "TeensySD");

    if (kSerialDiagnosticsEnabled) {
        Serial.println("[DIAG] MTP started, label=TeensySD");
    }
}

void loop() {
    MTP.loop();

    if (kSerialDiagnosticsEnabled && statusTimer > kSerialDiagHeartbeatMs) {
        statusTimer = 0;
        Serial.println("[DIAG] MTP service active");
    }
}