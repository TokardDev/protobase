/*
  ProtoHead — firmware de la tête (visière)

  ESP32 WROOM + shield "ESP32 HUB75 Driver" de rorosaurus (v1.1) + 2 dalles 64x32.
  Reçoit les appuis de bouton du gant en ESP-NOW et affiche la face correspondante.

  Fichiers :
    faces.h     généré par tools/png2layers.py à partir de PROTOfaces/ (ne pas éditer)
    layers.h    structures Layer / FaceDef
    protocol.h  format des messages gant -> tête (identique dans ProtoGlove/)

  Libs : "ESP32 HUB75 LED MATRIX PANEL DMA Display" (mrcodetastic) + "Adafruit GFX Library"
         + "ESP Async WebServer" et "Async TCP" (les deux par ESP32Async).
  Pas besoin de modifier la lib : le brochage du shield est défini ci-dessous.

  Mode upload (appui long sur le bouton 3 du gant) : la tête crée un WiFi "Proto-XXXX",
  et la page http://192.168.4.1/ permet d'envoyer de nouvelles faces depuis un téléphone.
  Elles sont gardées en flash (LittleFS) et rechargées au démarrage.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>  // police 3x5 d'Adafruit GFX : 16 caractères par dalle
#include <ESPAsyncWebServer.h>  // lib "ESP Async WebServer" (ESP32Async) + "Async TCP" (ESP32Async)
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#if __has_include(<esp_mac.h>)
  #include <esp_mac.h>  // esp_read_mac (ailleurs que dans esp_system.h selon la version)
#endif

#include "protocol.h"
#include "faces.h"
#include "webui.h"
#include "missingno.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copie ProtoHead/secrets.example.h en ProtoHead/secrets.h et choisis ton mot de passe WiFi"
#endif
static_assert(sizeof(UPLOAD_PASSWORD) - 1 >= 8, "UPLOAD_PASSWORD : 8 caractères minimum (WPA2)");
constexpr bool sameText(const char *a, const char *b) { return *a == *b && (*a == 0 || sameText(a + 1, b + 1)); }
static_assert(!sameText(UPLOAD_PASSWORD, "change-moi-vraiment"), "Change le mot de passe d'exemple dans secrets.h");

/* ---------------------------------------------------------------------------
   Matrice
   --------------------------------------------------------------------------- */
#define PANEL_W     64
#define PANEL_H     32
#define PANEL_CHAIN 2
#define SCREEN_W    (PANEL_W * PANEL_CHAIN)

// Brochage du shield rorosaurus v1.1 (= ESP32_FORUM_PINOUT de SmartMatrix)
#define R1_PIN  2
#define G1_PIN  15
#define B1_PIN  4
#define R2_PIN  16
#define G2_PIN  27
#define B2_PIN  17
#define A_PIN   5
#define B_PIN   18
#define C_PIN   19
#define D_PIN   21
#define E_PIN   -1   // câblé sur GPIO 12 par le shield, inutile pour une dalle 64x32 en 1/16 scan
#define LAT_PIN 26
#define OE_PIN  25
#define CLK_PIN 22

/* ---------------------------------------------------------------------------
   Réglages
   --------------------------------------------------------------------------- */
// Luminosité (0-255) par crans. Maintenir le bouton 0 monte d'un cran toutes les 250 ms,
// le bouton 1 descend ; ça s'arrête aux extrémités (pas de retour au début : on ne voit
// pas la visière quand on la porte). Tout le tableau se parcourt en ~2,5 s.
const uint8_t BRIGHTNESS_LEVELS[] = {8, 16, 28, 44, 64, 90, 120, 155, 195, 240};
const uint8_t BRIGHTNESS_COUNT    = sizeof(BRIGHTNESS_LEVELS);
const uint8_t DEFAULT_BRIGHTNESS  = 5;  // index dans le tableau ci-dessus (90 = l'ancien réglage)

const uint16_t BLINK_FRAME_MS   = 45;    // durée de chaque étape du clignement
const uint32_t BLINK_MIN_MS     = 2500;  // intervalle aléatoire entre deux clignements
const uint32_t BLINK_MAX_MS     = 6000;

// Le mot de passe du WiFi d'upload est dans secrets.h (hors du repo).
const uint32_t UPLOAD_TIMEOUT_MS = 10 * 60000;  // le mode upload se coupe après 10 min sans activité

// Ce que fait chaque bouton du gant (0..3), en appui court et en appui long.
enum ActionType : uint8_t { ACT_NONE, ACT_FACE, ACT_PANIC, ACT_BRIGHTER, ACT_DIMMER, ACT_UPLOAD };
struct ButtonAction { ActionType type; uint8_t face; };

const ButtonAction BUTTONS[] = {
  { ACT_FACE, FACE_NEUTRAL },
  { ACT_FACE, FACE_HAPPY },  // FACE_ANGRY existe aussi si tu veux la remettre
  { ACT_FACE, FACE_UWU },    // >w< (blush)
  { ACT_PANIC, 0 },          // crash + kernel panic, sur l'auriculaire
};
const ButtonAction LONG_PRESS[] = {
  { ACT_BRIGHTER, 0 },    // maintenir le bouton 0 : plus lumineux (jusqu'au max)
  { ACT_DIMMER, 0 },      // maintenir le bouton 1 : moins lumineux (jusqu'au min)
  { ACT_NONE, 0 },
  { ACT_UPLOAD, 0 },      // maintenir le bouton 3 : allume / coupe le mode upload
};
const uint8_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

// Kernel panic : un écran fixe de 5 lignes (police 3x5, 4 px par caractère, 14 max).
// Pas de défilement. Le texte est collé côté nez : la partie arrière des dalles,
// en partie cachée par le casque, ne reçoit que du vide. Le coin arrière-bas est le
// plus caché : les lignes du bas restent courtes (10 caractères max, curseur compris).
const char *const PANIC_LINES[] = {
  "Kernel panic!",
  "Cause: called",   // ASCII seulement : la police TomThumb n'a pas d'accents
  "a toaster",
  "again.",
  "Love that.",      // suivi du curseur clignotant
};
const uint8_t PANIC_LINE_COUNT = sizeof(PANIC_LINES) / sizeof(PANIC_LINES[0]);

/* ---------------------------------------------------------------------------
   État
   --------------------------------------------------------------------------- */
MatrixPanel_I2S_DMA *display = nullptr;
QueueHandle_t rxQueue;
FaceDef faces[FACE_COUNT];   // faces affichées : celles de faces.h, ou celles envoyées par le téléphone

enum EyePhase : uint8_t { EYE_IDLE, EYE_CLOSING, EYE_OPENING };

uint8_t  face        = FACE_NEUTRAL;
uint8_t  pendingFace = FACE_NEUTRAL;  // face à afficher au prochain oeil fermé
uint8_t  eyeFrame    = 0;             // 0 = ouvert, eyeFrames-1 = fermé
EyePhase eyePhase    = EYE_IDLE;
uint32_t nextEyeStep = 0;
uint32_t nextBlink   = 0;

bool     kpanic       = false;   // kernel panic affiché
uint8_t  panicPrinted = 0;       // nombre de lignes déjà "imprimées"
bool     panicCursor  = false;   // curseur clignotant une fois le panic fini
uint32_t nextPanicStep = 0;

uint8_t  brightness = DEFAULT_BRIGHTNESS;
uint32_t saveBrightnessAt = 0;   // sauvegarde différée de la luminosité (0 = rien à sauver)
Preferences prefs;
bool     dirty      = true;   // l'écran doit être redessiné
bool     uploadMode = false;  // WiFi d'upload actif
int16_t  lastSeq    = -1;

/* ---------------------------------------------------------------------------
   Dessin
   --------------------------------------------------------------------------- */
// Dessine un calque sur le premier panneau et en miroir sur le second.
// Comparaison qui reste juste quand millis() repasse à 0 (au bout de ~49 jours).
static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// Cible du dessin : l'écran, ou le buffer du crash quand drawToCrashBuf est vrai.
uint8_t crashBuf[PANEL_H][SCREEN_W][3];  // 12 Ko : image source de l'animation de crash
bool    drawToCrashBuf = false;

void putPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!drawToCrashBuf) {
    display->drawPixelRGB888(x, y, r, g, b);
    return;
  }
  crashBuf[y][x][0] = r;
  crashBuf[y][x][1] = g;
  crashBuf[y][x][2] = b;
}

void drawLayer(const Layer *l) {
  if (!l) return;
  const int span = max(1, l->x1 - l->x0);
  for (int y = 0; y < PANEL_H; y++) {
    const uint8_t *row = l->bits + y * (PANEL_W / 8);
    for (int x = 0; x < PANEL_W; x++) {
      if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;
      const int t = constrain((x - l->x0) * 256 / span, 0, 256);
      const uint8_t r = l->c0[0] + (l->c1[0] - l->c0[0]) * t / 256;
      const uint8_t g = l->c0[1] + (l->c1[1] - l->c0[1]) * t / 256;
      const uint8_t b = l->c0[2] + (l->c1[2] - l->c0[2]) * t / 256;
      putPixel(x, y, r, g, b);
      putPixel(SCREEN_W - 1 - x, y, r, g, b);
    }
  }
}

void drawFaceFrame(uint8_t id, uint8_t eye) {
  const FaceDef &f = faces[id];
  drawLayer(f.eye[min<uint8_t>(eye, f.eyeFrames - 1)]);
  drawLayer(f.mouth);
  drawLayer(f.nose);
}

void drawFace() { drawFaceFrame(face, eyeFrame); }

/* ---------------------------------------------------------------------------
   Crash : animation qui précède le kernel panic
   1. corruption de plus en plus forte de la face (bandes décalées, couleurs dédoublées,
      parasites, inversions, bouts d'autres faces, MissingNo qui surgit),
   2. extinction façon tube cathodique (écrasement en ligne, ligne -> point),
   3. noir, puis le texte du kernel panic.
   --------------------------------------------------------------------------- */
const uint16_t CRASH_GLITCH_MS   = 1400;
const uint16_t CRASH_COLLAPSE_MS = 400;
const uint16_t CRASH_BLACK_MS    = 500;
const uint8_t  CRASH_FRAME_MS    = 33;    // ~30 images/s

uint8_t  crashPhase = 0;     // 0 = pas de crash, 1 = corruption, 2 = extinction, 3 = noir
uint32_t crashStart = 0, nextCrashFrame = 0, crashT = 0;
uint32_t crashSeed = 1;      // graine de l'image en cours (réutilisée = image qui saccade)
uint32_t rng = 1;

uint32_t rnd() {             // xorshift : rapide et rejouable avec la même graine
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
bool chance(int percent) { return (int)(rnd() % 100) < percent; }

// MissingNo, sans miroir sur chaque dalle (pour qu'il soit dans le bon sens des deux
// côtés), collé côté nez, sur toute la hauteur.
void drawMissingno() {
  for (int p = 0; p < PANEL_CHAIN; p++) {
    const int ox = p == 0 ? PANEL_W - MISSINGNO_W - 6 : PANEL_W + 6;
    for (int y = 0; y < MISSINGNO_H; y++)
      for (int x = 0; x < MISSINGNO_W; x++) {
        switch (MISSINGNO[y][x]) {
          case 'L': putPixel(ox + x, y, 248, 232, 248); break;
          case 'P': putPixel(ox + x, y, 128, 112, 152); break;
          case 'O': putPixel(ox + x, y, 240, 176, 136); break;
          case 'K': putPixel(ox + x, y, 40, 24, 24);    break;  // pas noir pur : une LED éteinte ne se voit pas
        }
      }
  }
}

void startCrash() {
  kpanic = false;
  crashPhase = 1;
  crashStart = millis();
  nextCrashFrame = crashStart;
  crashSeed = esp_random() | 1;
  dirty = true;
}

void stopCrash() { crashPhase = 0; }

void updateCrash(uint32_t now) {
  if (!crashPhase || !reached(now, nextCrashFrame)) return;
  nextCrashFrame = now + CRASH_FRAME_MS;
  crashT = now - crashStart;
  if (crashT < CRASH_GLITCH_MS) {
    crashPhase = 1;
    // dans la 2e moitié, l'image se fige parfois : on rejoue la même graine
    rng = crashSeed;
    if (!(crashT > CRASH_GLITCH_MS / 2 && chance(30))) crashSeed = esp_random() | 1;
  } else if (crashT < CRASH_GLITCH_MS + CRASH_COLLAPSE_MS) {
    crashPhase = 2;
  } else if (crashT < CRASH_GLITCH_MS + CRASH_COLLAPSE_MS + CRASH_BLACK_MS) {
    crashPhase = 3;
  } else {
    crashPhase = 0;  // fin : le texte du kernel panic prend le relais
    kpanic = true;
    panicPrinted = 0;
    panicCursor = false;
    nextPanicStep = now + 200;
  }
  dirty = true;
}

// Remplit crashBuf avec la source de l'image : la face actuelle, parfois une autre face,
// et MissingNo à deux moments fixes (+ quelques flashs au hasard).
// Renvoie vrai si c'est MissingNo : il doit rester reconnaissable, donc peu d'effets.
bool fillCrashSource(int p100) {
  memset(crashBuf, 0, sizeof(crashBuf));
  drawToCrashBuf = true;
  const bool block = (p100 >= 52 && p100 < 60) || (p100 >= 84 && p100 < 87) || chance(p100 / 12);
  if (block) {
    drawMissingno();
  } else if (chance(8 + p100 / 3)) {
    const uint8_t other = rnd() % FACE_COUNT;
    drawFaceFrame(other, rnd() % faces[other].eyeFrames);
  } else {
    drawFaceFrame(face, eyeFrame);
  }
  drawToCrashBuf = false;
  return block;
}

void drawCrashGlitch() {
  const int p100 = crashT * 100 / CRASH_GLITCH_MS;  // 0 -> 100 : intensité
  rng = crashSeed;
  const bool clean = fillCrashSource(p100);
  if (!clean && chance(4 + p100 / 8)) return;  // image perdue : noir complet

  const bool    invert = !clean && chance(5 + p100 / 6);
  const uint8_t perm   = !clean && chance(8 + p100 / 4) ? 1 + rnd() % 2 : 0;  // canaux de couleur échangés
  const int     split  = !clean && chance(35 + p100 / 2) ? 1 + p100 / 34 : 0; // rouge et bleu décalés
  const int     maxShift = clean ? 2 : 1 + p100 * 16 / 100;
  const int     shiftChance = clean ? 12 : 15 + p100 * 6 / 10;
  int8_t shift[PANEL_H];
  for (int y = 0; y < PANEL_H;) {
    const int h = 1 + rnd() % 5;
    const int s = chance(shiftChance) ? (int)(rnd() % (2 * maxShift + 1)) - maxShift : 0;
    for (int i = 0; i < h && y < PANEL_H; i++, y++) shift[y] = s;
  }

  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < SCREEN_W; x++) {
      const int sx = x - shift[y];
      const int xr = sx + split, xb = sx - split;
      const uint8_t r = (xr >= 0 && xr < SCREEN_W) ? crashBuf[y][xr][0] : 0;
      const uint8_t g = (sx >= 0 && sx < SCREEN_W) ? crashBuf[y][sx][1] : 0;
      const uint8_t b = (xb >= 0 && xb < SCREEN_W) ? crashBuf[y][xb][2] : 0;
      if (!(r | g | b)) continue;
      uint8_t c[3] = {r, g, b};
      if (perm == 1) { c[0] = g; c[1] = b; c[2] = r; }
      if (perm == 2) { c[0] = b; c[1] = r; c[2] = g; }
      if (invert) for (int k = 0; k < 3; k++) c[k] = 255 - c[k];
      display->drawPixelRGB888(x, y, c[0], c[1], c[2]);
    }
  }

  static const uint8_t NOISE[4][3] = {{255, 255, 255}, {0, 255, 220}, {255, 0, 200}, {120, 255, 60}};
  const int dots = (clean ? p100 / 2 : p100 * p100) / 40;  // parasites : de rien à ~250
  for (int i = 0; i < dots; i++) {
    const uint8_t *n = NOISE[rnd() % 4];
    display->drawPixelRGB888(rnd() % SCREEN_W, rnd() % PANEL_H, n[0], n[1], n[2]);
  }
  if (chance(10 + p100 / 3)) {  // ligne morte horizontale
    const int y = rnd() % PANEL_H, x0 = rnd() % SCREEN_W, len = 8 + rnd() % 60;
    const uint8_t v = 120 + rnd() % 136;
    for (int x = x0; x < x0 + len && x < SCREEN_W; x++) display->drawPixelRGB888(x, y, v, v, v);
  }
}

// Extinction façon tube cathodique : l'image s'écrase en une ligne qui blanchit,
// la ligne rétrécit vers le nez, puis un point qui s'éteint.
void drawCrashCollapse() {
  const int q = (crashT - CRASH_GLITCH_MS) * 1000 / CRASH_COLLAPSE_MS;  // 0 -> 1000
  const float cy = (PANEL_H - 1) / 2.0f, cx = (SCREEN_W - 1) / 2.0f;
  if (q < 450) {
    const float a = q / 450.0f, scale = 1.0f - a * 0.97f;
    for (int y = 0; y < PANEL_H; y++) {
      const int sy = (int)lroundf(cy + (y - cy) / scale);
      if (sy < 0 || sy >= PANEL_H) continue;
      for (int x = 0; x < SCREEN_W; x++) {
        const uint8_t *s = crashBuf[sy][x];
        if (!(s[0] | s[1] | s[2])) continue;
        display->drawPixelRGB888(x, y, s[0] + (255 - s[0]) * a, s[1] + (255 - s[1]) * a, s[2] + (255 - s[2]) * a);
      }
    }
    const uint8_t v = 255 * a * a;  // la ligne centrale s'allume pendant l'écrasement
    for (int x = 0; x < SCREEN_W; x++) {
      display->drawPixelRGB888(x, 15, v, v, v);
      display->drawPixelRGB888(x, 16, v, v, v);
    }
  } else if (q < 850) {
    const float b = (q - 450) / 400.0f;
    const float half = max(1.0f, cx * (1.0f - b));
    for (int x = (int)(cx - half); x <= (int)(cx + half + 0.5f); x++) {
      if (x < 0 || x >= SCREEN_W) continue;
      display->drawPixelRGB888(x, 15, 255, 255, 255);
      if (b < 0.6f) display->drawPixelRGB888(x, 16, 200, 200, 200);
    }
  } else {
    const uint8_t v = 255 * (1000 - q) / 150;
    for (int x = 62; x <= 65; x++) display->drawPixelRGB888(x, 15, v, v, v);
  }
}

void drawCrash() {
  if (crashPhase == 1) drawCrashGlitch();
  else if (crashPhase == 2) drawCrashCollapse();
  // phase 3 : noir
}

// Même texte sur chaque dalle, sans miroir pour rester lisible. Sur la dalle 0 le
// côté nez est à droite (texte aligné à droite), sur la dalle 1 il est à gauche.
void drawPanic() {
  const uint16_t grey = display->color565(200, 200, 200);
  display->setFont(&TomThumb);
  display->setTextWrap(false);
  display->setTextColor(grey);
  for (int p = 0; p < PANEL_CHAIN; p++) {
    for (uint8_t i = 0; i < panicPrinted; i++) {
      const bool last = i == PANIC_LINE_COUNT - 1;
      const int  w    = 4 * (strlen(PANIC_LINES[i]) + (last ? 1 : 0));  // +1 case pour le curseur
      const int  x    = p == 0 ? PANEL_W - w : PANEL_W + 1;
      display->setCursor(x, 5 + 6 * i);  // y = ligne de base du texte
      display->print(PANIC_LINES[i]);
      if (last && panicCursor)
        display->fillRect(x + 4 * strlen(PANIC_LINES[i]), 6 * i, 3, 5, grey);
    }
  }
}

void render() {
  display->clearScreen();
  if (crashPhase)  drawCrash();
  else if (kpanic) drawPanic();
  else             drawFace();
  if (uploadMode) {  // petit point vert en haut côté nez (l'arrière des dalles est en partie caché)
    for (int y = 0; y < 2; y++)
      for (int x = PANEL_W - 2; x < PANEL_W; x++) {
        display->drawPixelRGB888(x, y, 0, 200, 60);
        display->drawPixelRGB888(SCREEN_W - 1 - x, y, 0, 200, 60);
      }
  }
  display->flipDMABuffer();
  dirty = false;
}

/* ---------------------------------------------------------------------------
   Animations
   --------------------------------------------------------------------------- */

void scheduleBlink(uint32_t now) {
  nextBlink = now + BLINK_MIN_MS + esp_random() % (BLINK_MAX_MS - BLINK_MIN_MS);
}

void updateEye(uint32_t now) {
  if (kpanic || crashPhase) return;
  const uint8_t closed = faces[face].eyeFrames - 1;

  if (eyePhase == EYE_IDLE) {
    if (pendingFace == face && !reached(now, nextBlink)) return;
    eyePhase = EYE_CLOSING;   // clignement normal, ou changement de face
    nextEyeStep = now;
  }
  if (!reached(now, nextEyeStep)) return;
  nextEyeStep = now + BLINK_FRAME_MS;

  if (eyePhase == EYE_CLOSING) {
    if (eyeFrame < closed) {
      eyeFrame++;
    } else {
      // oeil fermé : c'est là qu'on change de face, ça masque la transition
      face = pendingFace;
      eyeFrame = faces[face].eyeFrames - 1;
      eyePhase = EYE_OPENING;
    }
  } else {  // EYE_OPENING
    if (eyeFrame > 0) {
      eyeFrame--;
    } else {
      eyePhase = EYE_IDLE;
      scheduleBlink(now);
      return;
    }
  }
  dirty = true;
}

void updatePanic(uint32_t now) {
  if (!kpanic || !reached(now, nextPanicStep)) return;
  if (panicPrinted < PANIC_LINE_COUNT) {
    panicPrinted++;
    nextPanicStep = now + 250 + esp_random() % 250;  // les lignes apparaissent une à une
  } else {
    panicCursor = !panicCursor;                    // le système est figé, seul le curseur clignote
    nextPanicStep = now + 500;
  }
  dirty = true;
}

// Demande l'affichage d'une face : elle apparaît au prochain oeil fermé.
void selectFace(uint8_t id) {
  if (id >= FACE_COUNT) return;
  pendingFace = id;
  if (kpanic || crashPhase) {
    // sortie du crash / kernel panic : on "redémarre" avec l'oeil fermé qui s'ouvre
    stopCrash();
    kpanic = false;
    face = pendingFace;
    eyeFrame = faces[face].eyeFrames - 1;
    eyePhase = EYE_OPENING;
    nextEyeStep = millis() + BLINK_FRAME_MS;
    dirty = true;
  } else if (eyePhase == EYE_OPENING && pendingFace != face) {
    eyePhase = EYE_CLOSING;  // on referme direct pour changer de face
  }
}

/* ---------------------------------------------------------------------------
   Mémoire
   --------------------------------------------------------------------------- */
void printMemory(const char *when) {
  Serial.printf("[mémoire] %s : libre %u o, dont DMA %u o, plus gros bloc %u o\n", when,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* ---------------------------------------------------------------------------
   Faces envoyées depuis le téléphone + mode upload
   --------------------------------------------------------------------------- */
// Format d'une face (envoyé en hex par webui.h, stocké tel quel en flash) :
//   'P', 'F', 1, nbYeux, puis nbYeux+2 calques (yeux..., bouche, nez) de
//   x0, x1, c0[3], c1[3] + 256 octets de masque.
const size_t LAYER_BITS     = PANEL_W * PANEL_H / 8;
const size_t LAYER_BYTES    = 8 + LAYER_BITS;
const size_t FACE_HEADER    = 4;
const size_t FACE_MAX_BYTES = FACE_HEADER + (MAX_EYE_FRAMES + 2) * LAYER_BYTES;

struct CustomFace {
  uint8_t bits[MAX_EYE_FRAMES + 2][LAYER_BITS];
  Layer   layers[MAX_EYE_FRAMES + 2];
};
CustomFace customFaces[FACE_COUNT];
bool       isCustom[FACE_COUNT];

// Le serveur web est asynchrone : il gère plusieurs connexions à la fois (le navigateur
// d'un téléphone en ouvre plusieurs en parallèle, un serveur synchrone se bloque dessus).
// Ses callbacks tournent dans la tâche réseau, en parallèle de loop() : ils ne touchent
// jamais l'affichage, ils déposent une commande dans webQueue que loop() exécute.
AsyncWebServer server(80);
DNSServer dns;  // répond 192.168.4.1 à tous les noms : les tests de connexion du téléphone arrivent chez nous
bool      webStarted = false;
bool      fsOk = false;
volatile uint32_t uploadUntil = 0;

enum WebCmdType : uint8_t { WEB_SHOW, WEB_RESET, WEB_FACE };
struct WebCmd { WebCmdType type; uint8_t id; uint16_t len; };
QueueHandle_t webQueue;
uint8_t       webFaceBuf[FACE_MAX_BYTES];  // face reçue, en attente d'être appliquée par loop()
volatile bool webFaceBusy = false;

String facePath(uint8_t id) { return String("/face") + id + ".bin"; }

// Vérifie le format d'une face (sans rien modifier).
bool faceDataValid(const uint8_t *buf, size_t len) {
  if (len < FACE_HEADER) return false;
  const uint8_t n = buf[3];
  if (buf[0] != 'P' || buf[1] != 'F' || buf[2] != 1 || n < 1 || n > MAX_EYE_FRAMES) return false;
  return len == FACE_HEADER + (n + 2) * LAYER_BYTES;
}

bool applyFace(uint8_t id, const uint8_t *buf, size_t len) {
  if (id >= FACE_COUNT || !faceDataValid(buf, len)) return false;
  const uint8_t n = buf[3];

  CustomFace &c = customFaces[id];
  const uint8_t *p = buf + FACE_HEADER;
  for (uint8_t i = 0; i < n + 2; i++, p += LAYER_BYTES) {
    Layer &l = c.layers[i];
    l.x0 = p[0];
    l.x1 = p[1];
    memcpy(l.c0, p + 2, 3);
    memcpy(l.c1, p + 5, 3);
    memcpy(c.bits[i], p + 8, LAYER_BITS);
    l.bits = c.bits[i];
  }
  FaceDef &f = faces[id];
  for (uint8_t i = 0; i < MAX_EYE_FRAMES; i++) f.eye[i] = i < n ? &c.layers[i] : nullptr;
  f.eyeFrames = n;
  f.mouth = &c.layers[n];
  f.nose  = &c.layers[n + 1];
  isCustom[id] = true;

  if (face == id) {  // la face affichée vient de changer : on repart d'un oeil ouvert
    eyeFrame = 0;
    eyePhase = EYE_IDLE;
    scheduleBlink(millis());
    dirty = true;
  }
  return true;
}

void resetFace(uint8_t id) {
  faces[id] = FACES[id];
  isCustom[id] = false;
  if (fsOk) LittleFS.remove(facePath(id));
  if (face == id) {
    eyeFrame = 0;
    eyePhase = EYE_IDLE;
    dirty = true;
  }
}

void loadCustomFaces() {
  static uint8_t buf[FACE_MAX_BYTES];
  for (uint8_t id = 0; id < FACE_COUNT; id++) {
    File f = LittleFS.open(facePath(id), "r");
    if (!f) continue;
    const size_t len = f.read(buf, sizeof(buf));
    f.close();
    Serial.printf("face %s perso : %s\n", FACE_NAMES[id], applyFace(id, buf, len) ? "chargée" : "fichier invalide, ignorée");
  }
}

size_t hexDecode(const uint8_t *s, size_t slen, uint8_t *out, size_t max) {
  const size_t n = slen / 2;
  if (slen % 2 || n > max) return 0;
  auto nib = [](uint8_t c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
  };
  for (size_t i = 0; i < n; i++) {
    const int h = nib(s[2 * i]), l = nib(s[2 * i + 1]);
    if (h < 0 || l < 0) return 0;
    out[i] = (h << 4) | l;
  }
  return n;
}

void touchUpload(AsyncWebServerRequest *r) {
  uploadUntil = millis() + UPLOAD_TIMEOUT_MS;
  if (r) Serial.printf("HTTP %s %s\n", r->method() == HTTP_POST ? "POST" : "GET", r->url().c_str());
}

// Lit le paramètre ?id= ; répond 400 et renvoie -1 s'il est absent ou invalide.
int requestedFace(AsyncWebServerRequest *r) {
  touchUpload(r);
  const String arg = r->hasParam("id") ? r->getParam("id")->value() : String();
  bool digits = arg.length() > 0 && arg.length() <= 3;
  for (size_t i = 0; i < arg.length(); i++) digits &= isDigit(arg[i]);
  const int id = digits ? arg.toInt() : -1;  // "abc".toInt() vaudrait 0 : on refuse plutôt
  if (id < 0 || id >= FACE_COUNT) {
    r->send(400, "text/plain", "id de face invalide");
    return -1;
  }
  return id;
}

// (types simples, sans valeur par défaut : l'IDE Arduino génère les prototypes en haut du fichier)
bool queueWebCmd(uint8_t type, uint8_t id, uint16_t len) {
  const WebCmd c = {(WebCmdType)type, id, len};
  return xQueueSend(webQueue, &c, 0) == pdTRUE;
}

// Corps des POST /face : il arrive par morceaux, on l'accumule dans r->_tempObject
// (libéré automatiquement par la lib à la fin de la requête).
void onFaceBody(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
  if (total > 2 * FACE_MAX_BYTES) return;  // trop gros : la requête sera refusée
  if (index == 0) r->_tempObject = calloc(total + 1, 1);
  if (r->_tempObject && index + len <= total) memcpy((uint8_t *)r->_tempObject + index, data, len);
}

void onFaceUpload(AsyncWebServerRequest *r) {
  const int id = requestedFace(r);
  if (id < 0) return;
  const size_t hexLen = r->contentLength();
  if (!r->_tempObject || hexLen > 2 * FACE_MAX_BYTES) {
    r->send(400, "text/plain", "face invalide");
    return;
  }
  if (webFaceBusy) {  // la face précédente n'a pas encore été appliquée
    r->send(503, "text/plain", "occupé, réessaie");
    return;
  }
  const size_t len = hexDecode((const uint8_t *)r->_tempObject, hexLen, webFaceBuf, sizeof(webFaceBuf));
  if (!len || !faceDataValid(webFaceBuf, len)) {
    r->send(400, "text/plain", "face invalide");
    return;
  }
  webFaceBusy = true;
  if (!queueWebCmd(WEB_FACE, id, len)) {
    webFaceBusy = false;
    r->send(503, "text/plain", "occupé, réessaie");
    return;
  }
  r->send(200, "text/plain", fsOk ? "ok" : "ok, mais pas sauvegardée");
}

// Exécuté dans loop() : applique ce que les requêtes web ont demandé.
void processWebCommands() {
  WebCmd c;
  while (xQueueReceive(webQueue, &c, 0) == pdTRUE) {
    switch (c.type) {
      case WEB_SHOW:
        selectFace(c.id);
        break;
      case WEB_RESET:
        resetFace(c.id);
        break;
      case WEB_FACE: {
        const bool ok = applyFace(c.id, webFaceBuf, c.len);
        bool saved = false;
        if (ok && fsOk) {
          File f = LittleFS.open(facePath(c.id), "w");
          saved = f && f.write(webFaceBuf, c.len) == c.len;
          if (f) f.close();
        }
        webFaceBusy = false;
        if (ok) selectFace(c.id);
        Serial.printf("face %s reçue (%u octets)%s\n", FACE_NAMES[c.id], (unsigned)c.len,
                      !ok ? ", INVALIDE" : saved ? ", sauvegardée" : ", NON sauvegardée");
        break;
      }
    }
  }
}

void setupWebRoutes() {
  // Tests "ai-je internet ?" d'Android / iOS / Windows : on répond comme si oui, sinon le
  // téléphone peut envoyer ses requêtes par les données mobiles et la page ne charge jamais.
  auto noContent = [](AsyncWebServerRequest *r) { touchUpload(r); r->send(204); };
  server.on("/generate_204", HTTP_GET, noContent);
  server.on("/gen_204", HTTP_GET, noContent);
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    r->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    r->send(200, "text/plain", "Microsoft Connect Test");
  });
  // Firefox (detectportal.firefox.com)
  server.on("/success.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    r->send(200, "text/plain", "success\n");
  });
  server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    r->send(200, "text/html", "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>");
  });
  server.onNotFound([](AsyncWebServerRequest *r) {  // tout le reste : réponse immédiate
    touchUpload(r);
    r->send(404);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    r->send(200, "text/html", WEB_UI);
  });
  server.on("/faces", HTTP_GET, [](AsyncWebServerRequest *r) {
    touchUpload(r);
    String j = "{\"heap\":" + String(ESP.getFreeHeap()) + ",\"fs\":" + (fsOk ? "true" : "false") + ",\"faces\":[";
    for (uint8_t id = 0; id < FACE_COUNT; id++) {
      if (id) j += ',';
      j += "{\"id\":" + String(id) + ",\"name\":\"" + FACE_NAMES[id] + "\",\"custom\":" + (isCustom[id] ? "true" : "false") + "}";
    }
    r->send(200, "application/json", j + "]}");
  });
  server.on("/face", HTTP_POST, onFaceUpload, nullptr, onFaceBody);
  server.on("/show", HTTP_POST, [](AsyncWebServerRequest *r) {
    const int id = requestedFace(r);
    if (id < 0) return;
    queueWebCmd(WEB_SHOW, id, 0);
    r->send(200, "text/plain", "ok");
  });
  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
    const int id = requestedFace(r);
    if (id < 0) return;
    queueWebCmd(WEB_RESET, id, 0);
    r->send(200, "text/plain", "ok");
  });
}

// Adresse MAC WiFi lue dans l'eFuse de la puce : WiFi.macAddress() renvoie des zéros
// avec la carte ESP32 3.x tant que l'interface station n'est pas complètement lancée.
void readMac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

void startUpload() {
  uint8_t mac[6];
  readMac(mac);
  char ssid[16];
  snprintf(ssid, sizeof(ssid), "Proto-%02X%02X", mac[4], mac[5]);
  // même canal que l'ESP-NOW : le gant continue de fonctionner pendant l'upload.
  // 1 seul appareil connecté à la fois : si tu es dessus, personne d'autre ne peut entrer.
  if (!WiFi.softAP(ssid, UPLOAD_PASSWORD, PROTO_CHANNEL, 0, 1)) {
    Serial.println("Échec du point d'accès WiFi");
    return;
  }
  dns.start(53, "*", WiFi.softAPIP());
  if (!webStarted) {  // le serveur reste en écoute ensuite : sans point d'accès, personne ne peut le joindre
    server.begin();
    webStarted = true;
  }
  uploadMode = true;
  touchUpload(nullptr);
  dirty = true;
  Serial.printf("Mode upload : WiFi \"%s\", puis http://%s/\n", ssid, WiFi.softAPIP().toString().c_str());
  printMemory("mode upload");
}

void stopUpload() {
  dns.stop();
  WiFi.softAPdisconnect(true);  // repasse en simple station, l'ESP-NOW reste actif
  esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  uploadMode = false;
  dirty = true;
  Serial.println("Mode upload coupé");
}

/* ---------------------------------------------------------------------------
   Commandes du gant
   --------------------------------------------------------------------------- */
// Monte (+1) ou descend (-1) d'un cran, sans boucler. Sauvegardé 3 s après le dernier changement.
void changeBrightness(int step) {
  const int next = constrain((int)brightness + step, 0, BRIGHTNESS_COUNT - 1);
  if (next == brightness) {
    Serial.printf("luminosité déjà au %s\n", step > 0 ? "maximum" : "minimum");
    return;
  }
  brightness = next;
  display->setBrightness8(BRIGHTNESS_LEVELS[brightness]);
  saveBrightnessAt = millis() + 3000;
  if (!saveBrightnessAt) saveBrightnessAt = 1;
  Serial.printf("luminosité %u (cran %u/%u)\n", BRIGHTNESS_LEVELS[brightness], brightness + 1, BRIGHTNESS_COUNT);
}

void saveBrightnessIfDue(uint32_t now) {
  if (!saveBrightnessAt || !reached(now, saveBrightnessAt)) return;
  saveBrightnessAt = 0;
  prefs.putUChar("bright", brightness);
}

// kind : 0 = appui court, 1 = appui long, 2 = bouton toujours maintenu (répétition).
// (uint8_t et pas un enum : l'IDE Arduino génère les prototypes en haut du fichier)
void handleButton(uint8_t button, uint8_t kind) {
  if (button >= BUTTON_COUNT) return;
  const ButtonAction &a = kind == 0 ? BUTTONS[button] : LONG_PRESS[button];
  // la répétition ne sert qu'à la luminosité : pas de mode upload allumé/éteint en boucle
  if (kind == 2 && a.type != ACT_BRIGHTER && a.type != ACT_DIMMER) return;
  if (kind != 2) Serial.printf("bouton %u%s\n", button, kind == 1 ? " (long)" : "");

  switch (a.type) {
    case ACT_NONE:
      break;

    case ACT_FACE:
      selectFace(a.face);
      break;

    case ACT_PANIC:
      if (!crashPhase && !kpanic) startCrash();  // déjà en train de crasher : on laisse finir
      break;

    case ACT_BRIGHTER:
      changeBrightness(+1);
      break;

    case ACT_DIMMER:
      changeBrightness(-1);
      break;

    case ACT_UPLOAD:
      if (uploadMode) stopUpload();
      else            startUpload();
      break;
  }
}

// Callback ESP-NOW : tourne dans la tâche WiFi, donc on se contente de mettre
// le paquet en file d'attente et c'est loop() qui le traite.
// La signature a changé avec la carte ESP32 3.x : ça compile avec les deux.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define RECV_CB_ARGS const esp_now_recv_info_t *info, const uint8_t *data, int len
#else
  #define RECV_CB_ARGS const uint8_t *mac, const uint8_t *data, int len
#endif

volatile uint32_t rxIgnored = 0;  // paquets ESP-NOW reçus mais pas pour nous (diagnostic)

void onReceive(RECV_CB_ARGS) {
  Packet p;
  if (len != sizeof(Packet)) { rxIgnored++; return; }
  memcpy(&p, data, sizeof(p));
  if (p.magic != PROTO_MAGIC || p.id != PROTO_ID) { rxIgnored++; return; }
  xQueueSend(rxQueue, &p, 0);
}

void processPackets() {
  static uint32_t ignoredShown = 0;
  if (rxIgnored != ignoredShown) {
    ignoredShown = rxIgnored;
    Serial.printf("ESP-NOW : %u paquet(s) ignoré(s) (mauvais format ou PROTO_ID différent)\n", (unsigned)ignoredShown);
  }
  Packet p;
  while (xQueueReceive(rxQueue, &p, 0) == pdTRUE) {
    if (p.seq == lastSeq) continue;  // doublon (le gant envoie chaque appui plusieurs fois)
    lastSeq = p.seq;
    if (p.cmd == CMD_BUTTON)     handleButton(p.arg, 0);
    if (p.cmd == CMD_LONG_PRESS) handleButton(p.arg, 1);
    if (p.cmd == CMD_REPEAT)     handleButton(p.arg, 2);
  }
}

/* ---------------------------------------------------------------------------
   Setup / loop
   --------------------------------------------------------------------------- */
void setupDisplay() {
  HUB75_I2S_CFG::i2s_pins pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
                                  A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
                                  LAT_PIN, OE_PIN, CLK_PIN};
  HUB75_I2S_CFG cfg(PANEL_W, PANEL_H, PANEL_CHAIN, pins);
  cfg.double_buff = true;  // on dessine dans un buffer caché puis on bascule : pas de scintillement
  // cfg.clkphase = false; // à décommenter si l'image est décalée d'un pixel ou a des artefacts

  display = new MatrixPanel_I2S_DMA(cfg);
  if (!display->begin()) {
    Serial.println("Échec init matrice (mémoire ?)");
    while (true) delay(1000);
  }
  display->setBrightness8(BRIGHTNESS_LEVELS[brightness]);
  display->clearScreen();
  display->flipDMABuffer();
}

void setupEspNow() {
  startRadio();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Échec init ESP-NOW, redémarrage");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onReceive);
  uint8_t mac[6];
  readMac(mac);
  Serial.printf("Tête prête, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setup() {
  Serial.begin(115200);
  printMemory("démarrage");
  for (uint8_t id = 0; id < FACE_COUNT; id++) faces[id] = FACES[id];
  rxQueue = xQueueCreate(8, sizeof(Packet));
  webQueue = xQueueCreate(4, sizeof(WebCmd));

  prefs.begin("proto", false);  // réglages gardés en flash (NVS)
  brightness = min<uint8_t>(prefs.getUChar("bright", DEFAULT_BRIGHTNESS), BRIGHTNESS_COUNT - 1);

  setupDisplay();  // en premier : les buffers DMA des dalles veulent de la mémoire pas encore fragmentée
  printMemory("après les dalles");

  fsOk = LittleFS.begin(true);  // formate la partition au tout premier démarrage
  if (fsOk) loadCustomFaces();
  else      Serial.println("LittleFS indisponible : les faces envoyées ne survivront pas à un redémarrage");

  setupEspNow();
  setupWebRoutes();
  printMemory("après le WiFi");
  scheduleBlink(millis());
}

void loop() {
  const uint32_t now = millis();
  processPackets();
  processWebCommands();
  checkRadio();
  updateEye(now);
  updateCrash(now);
  updatePanic(now);
  saveBrightnessIfDue(now);
  if (uploadMode) {
    dns.processNextRequest();
    if (reached(now, uploadUntil)) stopUpload();
  }
  if (dirty) render();
  delay(1);
}
