#pragma once
#include <stdint.h>

// Un calque = un masque 1 bit de 64x32 (256 octets, MSB = pixel de gauche)
// + un dégradé horizontal de c0 (colonne x0) à c1 (colonne x1).
struct Layer {
  const uint8_t *bits;
  uint8_t x0, x1;
  uint8_t c0[3];
  uint8_t c1[3];
};

#define MAX_EYE_FRAMES 4

// Une face = les étapes de clignement de l'oeil (0 = ouvert, dernière = fermé)
// + la bouche + le nez.
struct FaceDef {
  const Layer *eye[MAX_EYE_FRAMES];
  uint8_t eyeFrames;
  const Layer *mouth;
  const Layer *nose;
};
