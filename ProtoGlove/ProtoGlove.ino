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

// Gant main gauche : bouton 0 (le plus utilisé) sur l'index, 3 sur l'auriculaire.
const uint8_t BUTTON_PINS[] = {25, 33, 19, 21};  // index, majeur, annulaire, auriculaire (vérifié au serial)
const uint8_t BUTTON_COUNT  = sizeof(BUTTON_PINS);

const uint16_t DEBOUNCE_MS   = 25;
const uint16_t LONG_PRESS_MS = 800;  // maintenu plus longtemps : appui long (et pas d'appui court)
const uint16_t REPEAT_MS     = 250;  // puis un CMD_REPEAT toutes les 250 ms tant que c'est maintenu
const uint16_t CHORD_MS      = 300;  // 2e doigt appuyé dans ce délai après le 1er : combo
const uint16_t DOUBLE_MS     = 250;  // même doigt ré-appuyé dans ce délai après relâchement : double appui
                                     // (du coup un appui simple n'est envoyé qu'après ce délai)
const uint8_t  REPEATS       = 3;    // le broadcast n'a pas d'accusé de réception : on envoie 3 fois

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Button {
  bool     stable;      // état validé (true = relâché, à cause du pull-up)
  bool     raw;         // dernière lecture
  uint32_t changedAt;   // date du dernier changement de lecture
  uint32_t pressedAt;   // date de l'appui validé
  bool     longSent;    // appui long déjà envoyé pour cet appui
  uint32_t repeatAt;    // date du dernier appui long / répétition envoyé
};
Button buttons[BUTTON_COUNT];

// Une "session" va du premier doigt appuyé jusqu'à ce que tous soient relâchés.
// Si un 2e doigt arrive vite, c'est un combo : un seul CMD_CHORD au relâchement,
// pas d'appui court ni long pour les doigts concernés.
uint8_t  sessionMask  = 0;
uint32_t sessionStart = 0;
bool     sessionChord = false;

// Appui simple en attente : on attend DOUBLE_MS pour voir si le même doigt revient.
int8_t   tapButton = -1;     // -1 = rien en attente
uint32_t tapAt     = 0;      // date du relâchement
bool     tapSecond = false;  // le doigt est revenu : c'est son 2e appui qui est en cours

void sendCommand(Command cmd, uint8_t index);

void flushTap() {            // l'appui en attente était bien un appui simple
  if (tapButton >= 0 && !tapSecond) sendCommand(CMD_BUTTON, tapButton);
  tapButton = -1;
  tapSecond = false;
}

uint8_t seq;

void sendCommand(Command cmd, uint8_t index) {
  Packet p = {PROTO_MAGIC, PROTO_ID, ++seq, cmd, index};
  esp_err_t err = ESP_OK;
  for (uint8_t i = 0; i < REPEATS; i++) {
    const esp_err_t e = esp_now_send(BROADCAST, (const uint8_t *)&p, sizeof(p));
    if (e != ESP_OK) err = e;
    delay(5);
  }
  if (err != ESP_OK) Serial.printf("Envoi ESP-NOW refusé : %s\n", esp_err_to_name(err));
  if (cmd == CMD_CHORD) Serial.printf("combo 0x%X envoyé (seq %u)\n", index, seq);
  else Serial.printf("bouton %u%s envoyé (seq %u)\n", index,
                     cmd == CMD_LONG_PRESS ? " (long)" : cmd == CMD_REPEAT ? " (maintenu)" :
                     cmd == CMD_DOUBLE ? " (double)" : "", seq);
}

void setup() {
  Serial.begin(115200);

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    buttons[i] = {true, true, 0, 0, false, 0};
  }

  startRadio();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Échec init ESP-NOW, redémarrage");
    delay(1000);
    ESP.restart();
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;  // 0 = canal actuel de la radio (réglé par startRadio)
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Échec ajout du peer broadcast");
  }

  seq = esp_random();  // évite que la tête prenne le 1er appui pour un doublon après un reboot du gant
  Serial.println("Gant prêt");
}

bool anyLongSent() {
  for (uint8_t i = 0; i < BUTTON_COUNT; i++)
    if ((sessionMask & (1 << i)) && buttons[i].longSent) return true;
  return false;
}

void loop() {
  const uint32_t now = millis();
  checkRadio();
  bool anyDown = false;
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    Button &b = buttons[i];
    const bool r = digitalRead(BUTTON_PINS[i]);
    if (r != b.raw) {
      b.raw = r;
      b.changedAt = now;
    } else if (r != b.stable && now - b.changedAt >= DEBOUNCE_MS) {
      b.stable = r;
      if (!r) {               // appui : on attend de savoir si c'est court, long, double ou un combo
        if (tapButton >= 0) {
          if (tapButton == i && !tapSecond && now - tapAt <= DOUBLE_MS) tapSecond = true;  // 2e appui
          else flushTap();    // un autre doigt : l'appui en attente était simple
        }
        if (!sessionMask) {
          sessionStart = now;
          sessionChord = false;
        }
        b.pressedAt = now;
        b.longSent = false;  // avant le test du combo : sinon l'appui long précédent de ce doigt le bloque
        const bool wasAlone = sessionMask != 0 && !sessionChord;
        sessionMask |= 1 << i;
        if (wasAlone && now - sessionStart <= CHORD_MS && !anyLongSent()) {
          sessionChord = true;
          tapButton = -1;     // le combo l'emporte sur un double appui en cours
          tapSecond = false;
        }
      } else if (!b.longSent && !(sessionChord && (sessionMask & (1 << i)))) {
        // relâché avant LONG_PRESS_MS, hors combo : appui court... ou fin d'un double appui
        if (tapSecond && tapButton == i) {
          sendCommand(CMD_DOUBLE, i);
          tapButton = -1;
          tapSecond = false;
        } else {
          if (tapButton >= 0 && tapButton != i) flushTap();  // un autre doigt attendait encore
          tapButton = i;      // on attend DOUBLE_MS avant de l'envoyer
          tapAt = now;
          tapSecond = false;
        }
      }
    } else if (sessionChord && (sessionMask & (1 << i))) {
      // doigt d'un combo : ni appui long ni répétition
    } else if (!b.stable && !b.longSent && now - b.pressedAt >= LONG_PRESS_MS) {
      if (tapSecond && tapButton == i) {  // appui puis maintien : c'est l'appui long qui compte
        tapButton = -1;
        tapSecond = false;
      }
      b.longSent = true;
      b.repeatAt = now;
      sendCommand(CMD_LONG_PRESS, i);
    } else if (!b.stable && b.longSent && now - b.repeatAt >= REPEAT_MS) {
      b.repeatAt = now;
      sendCommand(CMD_REPEAT, i);
    }
    if (!b.stable) anyDown = true;
  }
  if (tapButton >= 0 && !tapSecond && now - tapAt > DOUBLE_MS) flushTap();  // pas de 2e appui : simple
  if (sessionMask && !anyDown) {  // tous les doigts relâchés : fin de la session
    if (sessionChord) sendCommand(CMD_CHORD, sessionMask);
    sessionMask = 0;
    sessionChord = false;
  }
  delay(2);
}
