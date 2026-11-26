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
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#if __has_include(<esp_mac.h>)
  #include <esp_mac.h>  // esp_read_mac (ailleurs que dans esp_system.h selon la version)
#endif

#include "protocol.h"
#include "faces.h"
#include "webui.h"

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
const uint8_t BRIGHTNESS_LEVELS[] = {30, 60, 90, 140, 200};  // 0-255
const uint8_t DEFAULT_BRIGHTNESS  = 2;                      // index dans le tableau ci-dessus

const uint16_t BLINK_FRAME_MS   = 45;    // durée de chaque étape du clignement
const uint32_t BLINK_MIN_MS     = 2500;  // intervalle aléatoire entre deux clignements
const uint32_t BLINK_MAX_MS     = 6000;

// Le mot de passe du WiFi d'upload est dans secrets.h (hors du repo).
const uint32_t UPLOAD_TIMEOUT_MS = 10 * 60000;  // le mode upload se coupe après 10 min sans activité

// Ce que fait chaque bouton du gant (0..3), en appui court et en appui long.
enum ActionType : uint8_t { ACT_NONE, ACT_FACE, ACT_PANIC, ACT_BRIGHTNESS, ACT_UPLOAD };
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
  { ACT_UPLOAD, 0 },      // maintenir le bouton 3 : allume / coupe le mode upload
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
  "Cause: someone",     // ASCII seulement : la police TomThumb n'a pas d'accents
  "called me a",
  "toaster again.",
  "Love that.",
  "---[ end ]---",      // les 5 dernières lignes restent affichées une fois figé
};
const uint8_t PANIC_LINE_COUNT = sizeof(PANIC_LINES) / sizeof(PANIC_LINES[0]);
const uint8_t PANIC_VISIBLE    = 5;

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
bool     dirty      = true;   // l'écran doit être redessiné
bool     uploadMode = false;  // WiFi d'upload actif
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
  const FaceDef &f = faces[face];
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
      if (x + 3 <= (p + 1) * PANEL_W)  // pas de curseur qui déborde sur l'autre dalle
        display->fillRect(x, 6 * line, 3, 5, display->color565(200, 200, 200));
    }
  }
}

void render() {
  display->clearScreen();
  if (kpanic) drawPanic();
  else      drawFace();
  if (uploadMode) {  // petit point vert dans les coins : le WiFi d'upload est allumé
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 2; x++) {
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
// Comparaison qui reste juste quand millis() repasse à 0 (au bout de ~49 jours).
static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

void scheduleBlink(uint32_t now) {
  nextBlink = now + BLINK_MIN_MS + esp_random() % (BLINK_MAX_MS - BLINK_MIN_MS);
}

void updateEye(uint32_t now) {
  if (kpanic) return;
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
    nextPanicStep = now + 90 + esp_random() % 220;  // vitesse d'impression irrégulière
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
  if (kpanic) {
    // sortie du kernel panic : on "redémarre" avec l'oeil fermé qui s'ouvre
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

WebServer server(80);
bool      fsOk = false;
uint32_t  uploadUntil = 0;

String facePath(uint8_t id) { return String("/face") + id + ".bin"; }

bool applyFace(uint8_t id, const uint8_t *buf, size_t len) {
  if (id >= FACE_COUNT || len < FACE_HEADER) return false;
  const uint8_t n = buf[3];
  if (buf[0] != 'P' || buf[1] != 'F' || buf[2] != 1 || n < 1 || n > MAX_EYE_FRAMES) return false;
  if (len != FACE_HEADER + (n + 2) * LAYER_BYTES) return false;

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

size_t hexDecode(const String &s, uint8_t *out, size_t max) {
  const size_t n = s.length() / 2;
  if (s.length() % 2 || n > max) return 0;
  auto nib = [](char c) -> int {
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

void touchUpload() { uploadUntil = millis() + UPLOAD_TIMEOUT_MS; }

// Lit le paramètre ?id= ; répond 400 et renvoie -1 s'il est absent ou invalide.
int requestedFace() {
  const String arg = server.arg("id");
  bool digits = arg.length() > 0 && arg.length() <= 3;
  for (size_t i = 0; i < arg.length(); i++) digits &= isDigit(arg[i]);
  const int id = digits ? arg.toInt() : -1;  // "abc".toInt() vaudrait 0 : on refuse plutôt
  if (id < 0 || id >= FACE_COUNT) {
    server.send(400, "text/plain", "id de face invalide");
    return -1;
  }
  touchUpload();
  return id;
}

void setupWebRoutes() {
  server.on("/", HTTP_GET, [] {
    touchUpload();
    server.send_P(200, "text/html", WEB_UI);
  });
  server.on("/faces", HTTP_GET, [] {
    touchUpload();
    String j = "{\"heap\":" + String(ESP.getFreeHeap()) + ",\"fs\":" + (fsOk ? "true" : "false") + ",\"faces\":[";
    for (uint8_t id = 0; id < FACE_COUNT; id++) {
      if (id) j += ',';
      j += "{\"id\":" + String(id) + ",\"name\":\"" + FACE_NAMES[id] + "\",\"custom\":" + (isCustom[id] ? "true" : "false") + "}";
    }
    server.send(200, "application/json", j + "]}");
  });
  server.on("/face", HTTP_POST, [] {
    const int id = requestedFace();
    if (id < 0) return;
    static uint8_t buf[FACE_MAX_BYTES];
    const size_t len = hexDecode(server.arg("plain"), buf, sizeof(buf));
    if (!len || !applyFace(id, buf, len)) {
      server.send(400, "text/plain", "face invalide");
      return;
    }
    bool saved = false;
    if (fsOk) {
      File f = LittleFS.open(facePath(id), "w");
      saved = f && f.write(buf, len) == len;
      if (f) f.close();
    }
    selectFace(id);
    Serial.printf("face %s reçue (%u octets)%s\n", FACE_NAMES[id], (unsigned)len, saved ? ", sauvegardée" : ", NON sauvegardée");
    server.send(200, "text/plain", saved ? "ok" : "ok, mais pas sauvegardée");
  });
  server.on("/show", HTTP_POST, [] {
    const int id = requestedFace();
    if (id < 0) return;
    selectFace(id);
    server.send(200, "text/plain", "ok");
  });
  server.on("/reset", HTTP_POST, [] {
    const int id = requestedFace();
    if (id < 0) return;
    resetFace(id);
    server.send(200, "text/plain", "ok");
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
  server.begin();
  uploadMode = true;
  touchUpload();
  dirty = true;
  Serial.printf("Mode upload : WiFi \"%s\", puis http://%s/\n", ssid, WiFi.softAPIP().toString().c_str());
  printMemory("mode upload");
}

void stopUpload() {
  server.stop();
  WiFi.softAPdisconnect(true);  // repasse en simple station, l'ESP-NOW reste actif
  esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  uploadMode = false;
  dirty = true;
  Serial.println("Mode upload coupé");
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
      selectFace(a.face);
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
  WiFi.persistent(false);  // ne pas réécrire la config WiFi en flash à chaque mode upload
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);    // radio toujours à l'écoute, sinon des appuis peuvent se perdre
  esp_wifi_set_channel(PROTO_CHANNEL, WIFI_SECOND_CHAN_NONE);

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
  updateEye(now);
  updatePanic(now);
  if (uploadMode) {
    server.handleClient();
    if (reached(now, uploadUntil)) stopUpload();
  }
  if (dirty) render();
  delay(1);
}
