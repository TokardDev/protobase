#pragma once
#include <stdint.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

// Format des messages ESP-NOW gant -> tête.
// ⚠ Ce fichier doit être IDENTIQUE dans ProtoHead/ et ProtoGlove/.

#define PROTO_MAGIC   0x50   // 'P' : filtre les paquets ESP-NOW qui ne viennent pas de nous
#define PROTO_ID      0x2A   // change-le (des deux côtés) si un autre proto avec ce code traîne à côté
#define PROTO_CHANNEL 1      // canal WiFi, identique des deux côtés

enum Command : uint8_t {
  CMD_BUTTON     = 1,  // arg = numéro du bouton appuyé (0..3). C'est la tête qui décide quoi en faire.
  CMD_LONG_PRESS = 2,  // arg = numéro du bouton maintenu (dans ce cas, pas de CMD_BUTTON)
};

// Démarre le WiFi en station sur PROTO_CHANNEL, prêt pour l'ESP-NOW.
// Même code sur le gant et la tête : ils doivent être sur le même canal.
inline void startRadio() {
  WiFi.persistent(false);  // ne rien écrire en flash
  WiFi.mode(WIFI_STA);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // en 3.x le WiFi démarre en tâche de fond : régler le canal avant la fin du démarrage échoue
  for (int i = 0; i < 200 && !WiFi.STA.started(); i++) delay(10);
#endif
  WiFi.disconnect();
  WiFi.setSleep(false);    // radio toujours à l'écoute
  const esp_err_t err = esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  uint8_t channel = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&channel, &second);
  Serial.printf("Radio : canal %u%s\n", channel,
                err == ESP_OK && channel == PROTO_CHANNEL ? "" : "  <-- PAS LE BON CANAL, le gant et la tête ne s'entendront pas");
}

struct __attribute__((packed)) Packet {
  uint8_t magic;  // PROTO_MAGIC
  uint8_t id;     // PROTO_ID
  uint8_t seq;    // numéro d'appui : chaque appui est envoyé plusieurs fois, la tête ignore les doublons
  uint8_t cmd;    // Command
  uint8_t arg;
};
