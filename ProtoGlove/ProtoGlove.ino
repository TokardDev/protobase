/*
  ProtoGlove — firmware du gant

  ESP32 + 4 boutons câblés entre la pin et GND (pull-up interne).
  Chaque appui est envoyé à la tête en ESP-NOW broadcast : pas besoin de
  connaître l'adresse MAC de la tête ni de scanner le WiFi.
  C'est la tête qui décide ce que fait chaque bouton (voir BUTTONS dans ProtoHead.ino).
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"

const uint8_t BUTTON_PINS[] = {21, 19, 25, 33};  // bouton 0, 1, 2, 3
const uint8_t BUTTON_COUNT  = sizeof(BUTTON_PINS);

const uint16_t DEBOUNCE_MS = 25;
const uint8_t  REPEATS     = 3;   // le broadcast n'a pas d'accusé de réception : on envoie 3 fois

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Button {
  bool     stable;      // état validé (true = relâché, à cause du pull-up)
  bool     raw;         // dernière lecture
  uint32_t changedAt;   // date du dernier changement de lecture
};
Button buttons[BUTTON_COUNT];

uint8_t seq;

void sendButton(uint8_t index) {
  Packet p = {PROTO_MAGIC, PROTO_ID, ++seq, CMD_BUTTON, index};
  for (uint8_t i = 0; i < REPEATS; i++) {
    esp_now_send(BROADCAST, (const uint8_t *)&p, sizeof(p));
    delay(5);
  }
  Serial.printf("bouton %u envoyé (seq %u)\n", index, seq);
}

void setup() {
  Serial.begin(115200);

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    buttons[i] = {true, true, 0};
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Échec init ESP-NOW, redémarrage");
    delay(1000);
    ESP.restart();
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = PROTO_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Échec ajout du peer broadcast");
  }

  seq = esp_random();  // évite que la tête prenne le 1er appui pour un doublon après un reboot du gant
  Serial.println("Gant prêt");
}

void loop() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    Button &b = buttons[i];
    const bool r = digitalRead(BUTTON_PINS[i]);
    if (r != b.raw) {
      b.raw = r;
      b.changedAt = now;
    } else if (r != b.stable && now - b.changedAt >= DEBOUNCE_MS) {
      b.stable = r;
      if (!r) sendButton(i);  // front descendant = appui
    }
  }
  delay(2);
}
