#pragma once
#include <stdint.h>

// Format des messages ESP-NOW gant -> tête.
// ⚠ Ce fichier doit être IDENTIQUE dans ProtoHead/ et ProtoGlove/.

#define PROTO_MAGIC   0x50   // 'P' : filtre les paquets ESP-NOW qui ne viennent pas de nous
#define PROTO_ID      0x2A   // change-le (des deux côtés) si un autre proto avec ce code traîne à côté
#define PROTO_CHANNEL 1      // canal WiFi, identique des deux côtés

enum Command : uint8_t {
  CMD_BUTTON     = 1,  // arg = numéro du bouton appuyé (0..3). C'est la tête qui décide quoi en faire.
  CMD_LONG_PRESS = 2,  // arg = numéro du bouton maintenu (envoyé en plus de CMD_BUTTON)
};

struct __attribute__((packed)) Packet {
  uint8_t magic;  // PROTO_MAGIC
  uint8_t id;     // PROTO_ID
  uint8_t seq;    // numéro d'appui : chaque appui est envoyé plusieurs fois, la tête ignore les doublons
  uint8_t cmd;    // Command
  uint8_t arg;
};
