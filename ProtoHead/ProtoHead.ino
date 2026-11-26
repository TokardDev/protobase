/*
  ProtoHead — firmware de la tête (visière)

  ESP32 WROOM + shield "ESP32 HUB75 Driver" de rorosaurus (v1.1) + 2 dalles 64x32.
  Reçoit les appuis de bouton du gant en ESP-NOW et affiche la face correspondante.

  Fichiers :
    faces.h     généré par tools/png2layers.py à partir de PROTOfaces/ (ne pas éditer)
    layers.h    structures Layer / FaceDef
    protocol.h  format des messages gant -> tête (identique dans ProtoGlove/)

  Libs : "ESP32 HUB75 LED MATRIX PANEL DMA Display" (mrcodetastic) + "Adafruit GFX Library".
  Pas besoin de modifier la lib : le brochage du shield est défini ci-dessous.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>  // police 3x5 d'Adafruit GFX : 16 caractères par dalle

#include "protocol.h"
#include "faces.h"

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
const uint8_t BRIGHTNESS_LEVELS[] = {30, 60, 90, 140, 200};  // 0-255
const uint8_t DEFAULT_BRIGHTNESS  = 2;                      // index dans le tableau ci-dessus

const uint16_t BLINK_FRAME_MS   = 45;    // durée de chaque étape du clignement
const uint32_t BLINK_MIN_MS     = 2500;  // intervalle aléatoire entre deux clignements
const uint32_t BLINK_MAX_MS     = 6000;

// Ce que fait chaque bouton du gant (0..3), en appui court et en appui long.
enum ActionType : uint8_t { ACT_NONE, ACT_FACE, ACT_PANIC, ACT_BRIGHTNESS };
struct ButtonAction { ActionType type; uint8_t face; };

const ButtonAction BUTTONS[] = {
  { ACT_FACE, FACE_NEUTRAL },
  { ACT_FACE, FACE_HAPPY },  // FACE_ANGRY existe aussi si tu veux la remettre
  { ACT_PANIC, 0 },
  { ACT_FACE, FACE_UWU },
};
const ButtonAction LONG_PRESS[] = {
  { ACT_BRIGHTNESS, 0 },  // maintenir le bouton 0 : luminosité suivante
  { ACT_NONE, 0 },
  { ACT_NONE, 0 },
  { ACT_NONE, 0 },
};
const uint8_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

// Kernel panic : 16 caractères max par ligne, 5 lignes visibles par dalle.
const char *const PANIC_LINES[] = {
  "Kernel panic -",
  "not syncing:",
  "Attempted to",
  "kill init!",
  "exitcode=0x0b",
  "CPU: 0 PID: 1",
  "Comm: protod",
  "Call Trace:",
  " dump_stack+0x6d",
  " panic+0x101/0x2",
  " do_exit+0xb43/0",
  " face_init+0x1f",
  "---[ end Kernel",
  "panic ]---",
};
const uint8_t PANIC_LINE_COUNT = sizeof(PANIC_LINES) / sizeof(PANIC_LINES[0]);
const uint8_t PANIC_VISIBLE    = 5;

/* ---------------------------------------------------------------------------
   État
   --------------------------------------------------------------------------- */
MatrixPanel_I2S_DMA *display = nullptr;
QueueHandle_t rxQueue;

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
bool     dirty      = true;   // l'écran doit être redessiné
int16_t  lastSeq    = -1;

/* ---------------------------------------------------------------------------
   Dessin
   --------------------------------------------------------------------------- */
// Dessine un calque sur le premier panneau et en miroir sur le second.
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
      display->drawPixelRGB888(x, y, r, g, b);
      display->drawPixelRGB888(SCREEN_W - 1 - x, y, r, g, b);
    }
  }
}

void drawFace() {
  const FaceDef &f = FACES[face];
  drawLayer(f.eye[min<uint8_t>(eyeFrame, f.eyeFrames - 1)]);
  drawLayer(f.mouth);
  drawLayer(f.nose);
}

// Les lignes défilent comme sur une console. Même texte sur chaque dalle,
// sans miroir, pour qu'il reste lisible des deux côtés.
void drawPanic() {
  display->setFont(&TomThumb);
  display->setTextWrap(false);
  display->setTextColor(display->color565(200, 200, 200));
  const uint8_t first = panicPrinted > PANIC_VISIBLE ? panicPrinted - PANIC_VISIBLE : 0;
  for (int p = 0; p < PANEL_CHAIN; p++) {
    const int ox = p * PANEL_W + 1;
    for (uint8_t i = first; i < panicPrinted; i++) {
      display->setCursor(ox, 5 + 6 * (i - first));  // y = ligne de base du texte
      display->print(PANIC_LINES[i]);
    }
    if (panicCursor) {
      const int line = panicPrinted - first - 1;
      const int x = ox + 4 * strlen(PANIC_LINES[panicPrinted - 1]) + 1;
      display->fillRect(x, 6 * line, 3, 5, display->color565(200, 200, 200));
    }
  }
}

void render() {
  display->clearScreen();
  if (kpanic) drawPanic();
  else      drawFace();
  display->flipDMABuffer();
  dirty = false;
}

/* ---------------------------------------------------------------------------
   Animations
   --------------------------------------------------------------------------- */
// Comparaison qui reste juste quand millis() repasse à 0 (au bout de ~49 jours).
static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

void scheduleBlink(uint32_t now) {
  nextBlink = now + BLINK_MIN_MS + esp_random() % (BLINK_MAX_MS - BLINK_MIN_MS);
}

void updateEye(uint32_t now) {
  if (kpanic) return;
  const uint8_t closed = FACES[face].eyeFrames - 1;

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
      eyeFrame = FACES[face].eyeFrames - 1;
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
    nextPanicStep = now + 90 + esp_random() % 220;  // vitesse d'impression irrégulière
  } else {
    panicCursor = !panicCursor;                    // le système est figé, seul le curseur clignote
    nextPanicStep = now + 500;
  }
  dirty = true;
}

/* ---------------------------------------------------------------------------
   Commandes du gant
   --------------------------------------------------------------------------- */
void handleButton(uint8_t button, bool longPress) {
  if (button >= BUTTON_COUNT) return;
  const ButtonAction &a = longPress ? LONG_PRESS[button] : BUTTONS[button];
  Serial.printf("bouton %u%s\n", button, longPress ? " (long)" : "");

  switch (a.type) {
    case ACT_NONE:
      break;

    case ACT_FACE:
      if (a.face >= FACE_COUNT) return;
      pendingFace = a.face;
      if (kpanic) {
        // sortie du kernel panic : on "redémarre" avec l'oeil fermé qui s'ouvre
        kpanic = false;
        face = pendingFace;
        eyeFrame = FACES[face].eyeFrames - 1;
        eyePhase = EYE_OPENING;
        nextEyeStep = millis() + BLINK_FRAME_MS;
        dirty = true;
      } else if (eyePhase == EYE_OPENING && pendingFace != face) {
        eyePhase = EYE_CLOSING;  // on referme direct pour changer de face
      }
      break;

    case ACT_PANIC:
      kpanic = true;
      panicPrinted = 0;
      panicCursor = false;
      nextPanicStep = millis() + 150;
      dirty = true;
      break;

    case ACT_BRIGHTNESS:
      brightness = (brightness + 1) % sizeof(BRIGHTNESS_LEVELS);
      display->setBrightness8(BRIGHTNESS_LEVELS[brightness]);
      Serial.printf("luminosité %u\n", BRIGHTNESS_LEVELS[brightness]);
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

void onReceive(RECV_CB_ARGS) {
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PROTO_MAGIC || p.id != PROTO_ID) return;
  xQueueSend(rxQueue, &p, 0);
}

void processPackets() {
  Packet p;
  while (xQueueReceive(rxQueue, &p, 0) == pdTRUE) {
    if (p.seq == lastSeq) continue;  // doublon (le gant envoie chaque appui plusieurs fois)
    lastSeq = p.seq;
    if (p.cmd == CMD_BUTTON)     handleButton(p.arg, false);
    if (p.cmd == CMD_LONG_PRESS) handleButton(p.arg, true);
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
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);  // radio toujours à l'écoute, sinon des appuis peuvent se perdre

  if (esp_now_init() != ESP_OK) {
    Serial.println("Échec init ESP-NOW, redémarrage");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onReceive);
  Serial.print("Tête prête, MAC ");
  Serial.println(WiFi.macAddress());
}

void setup() {
  Serial.begin(115200);
  rxQueue = xQueueCreate(8, sizeof(Packet));
  setupDisplay();
  setupEspNow();
  scheduleBlink(millis());
}

void loop() {
  const uint32_t now = millis();
  processPackets();
  updateEye(now);
  updatePanic(now);
  if (dirty) render();
  delay(1);
}
