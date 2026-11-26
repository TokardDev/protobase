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
  CMD_REPEAT     = 3,  // arg = numéro du bouton toujours maintenu, envoyé à intervalle régulier
  CMD_CHORD      = 4,  // arg = masque des boutons appuyés ensemble (bit 0 = bouton 0...)
};

// Démarre le WiFi en station sur PROTO_CHANNEL, prêt pour l'ESP-NOW.
// Même code sur le gant et la tête : ils doivent être sur le même canal.
// La station ne doit JAMAIS chercher à se connecter à un réseau : pour ça elle scanne
// tous les canaux, et pendant ce temps elle rate l'ESP-NOW et les clients du WiFi d'upload.
inline void startRadio() {
  WiFi.persistent(false);       // ne rien écrire en flash
  WiFi.setAutoReconnect(false); // pas de reconnexion automatique (= pas de scan)
  WiFi.mode(WIFI_STA);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // en 3.x le WiFi démarre en tâche de fond : régler le canal avant la fin du démarrage échoue
  for (int i = 0; i < 200 && !WiFi.STA.started(); i++) delay(10);
#endif
  // Un réseau enregistré par un ancien firmware ? On l'oublie pour cette session.
  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
    Serial.printf("Radio : réseau WiFi enregistré \"%s\" ignoré\n", (const char *)conf.sta.ssid);
    esp_wifi_disconnect();
    wifi_config_t empty = {};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
  }
  WiFi.setSleep(false);         // radio toujours à l'écoute
  const esp_err_t err = esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  uint8_t channel = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&channel, &second);
  Serial.printf("Radio : canal %u%s\n", channel,
                err == ESP_OK && channel == PROTO_CHANNEL ? "" : "  <-- PAS LE BON CANAL, le gant et la tête ne s'entendront pas");
}

// À appeler dans loop() : signale si la radio quitte le canal (ce qui casse l'ESP-NOW).
inline void checkRadio() {
  static uint32_t last = 0;
  static uint8_t  shown = PROTO_CHANNEL;
  if (millis() - last < 1000) return;
  last = millis();
  uint8_t channel = 0;
  wifi_second_chan_t second;
  if (esp_wifi_get_channel(&channel, &second) != ESP_OK || channel == shown) return;
  shown = channel;
  Serial.printf("Radio : maintenant sur le canal %u%s\n", channel,
                channel == PROTO_CHANNEL ? " (revenue)" : "  <-- la radio a quitté le canal ESP-NOW");
}

struct __attribute__((packed)) Packet {
  uint8_t magic;  // PROTO_MAGIC
  uint8_t id;     // PROTO_ID
  uint8_t seq;    // numéro d'appui : chaque appui est envoyé plusieurs fois, la tête ignore les doublons
  uint8_t cmd;    // Command
  uint8_t arg;
};
