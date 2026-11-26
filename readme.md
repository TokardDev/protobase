# ProtoBase

Firmware for a protogen visor: two 64x32 HUB75 LED panels driven by an ESP32, with faces switched from a 4-button glove over ESP-NOW.

## Hardware

| Part | Notes |
|---|---|
| Head: ESP32 WROOM (DevKit V1) | plugged into the shield below |
| [ESP32 HUB75 Driver by rorosaurus](https://github.com/rorosaurus/esp32-hub75-driver) (v1.1) | ESP32 → HUB75 adapter |
| 2× P3 64x32 HUB75 panels, 1/16 scan | chained, the second one shows the mirror image |
| Glove: ESP32 + 4 push buttons | buttons between GPIO 21 / 19 / 25 / 33 and GND |
| 5 V power supply | the panels draw a lot, power them directly, not through the ESP32 |

### Shield pinout

The rorosaurus shield uses SmartMatrix's `ESP32_FORUM_PINOUT`, **not** the default pins of the DMA library. The pins are set in `ProtoHead/ProtoHead.ino`, so the library doesn't need to be modified.

| R1 | G1 | B1 | R2 | G2 | B2 | A | B | C | D | E | LAT | OE | CLK |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 15 | 4 | 16 | 27 | 17 | 5 | 18 | 19 | 21 | (12, unused) | 26 | 25 | 22 |

Free GPIOs on the shield's breakout header: 13, 14, 23, 32, 33 (input/output) and 34, 35, 36, 39 (input only, **no internal pull-up**).

## Software

1. Arduino IDE with the **esp32 by Espressif** board package (2.x or 3.x both work).
2. Libraries (Library Manager):
   - *ESP32 HUB75 LED MATRIX PANEL DMA Display* (mrcodetastic)
   - *Adafruit GFX Library*
3. Copy `ProtoHead/secrets.example.h` to `ProtoHead/secrets.h` and set the upload WiFi password.
4. Flash `ProtoHead/` on the head ESP32 and `ProtoGlove/` on the glove ESP32.

## Buttons

Each glove button is mapped in the `BUTTONS` (short press) and `LONG_PRESS` (held 0.8 s) tables at the top of `ProtoHead.ino`. A short press is sent when the button is released, so a long press never triggers the short-press action:

| Button | Press | Hold |
|---|---|---|
| 0 | neutral face (purple) | cycle brightness |
| 1 | happy face (cyan) | – |
| 2 | kernel panic | – |
| 3 | `>w<` face (pink) | upload mode on / off |

The angry face (orange) is still built into the firmware: put `FACE_ANGRY` in the table to use it.

The eyes blink on their own at random intervals, and face changes happen while the eye is closed.

## Uploading faces from a phone

Hold glove button 3 to turn on upload mode: a small green dot lights up in the top corners, and the head creates a WiFi network `Proto-XXXX` that accepts a single device at a time. Its password lives in `ProtoHead/secrets.h`, which git ignores: copy `ProtoHead/secrets.example.h` to `ProtoHead/secrets.h` and set your own password before compiling (the build stops with an error until you do). Connect your phone to it and open http://192.168.4.1/. On Android, accept "stay connected" when the phone warns that the network has no internet access (or turn mobile data off for a moment), otherwise it sends the requests over mobile data and the page can't reach the head.

On that page, pick the face to replace and select its PNGs (same format as below, one per blink step). The PNGs are converted in the browser with the same algorithm as `tools/png2layers.py`, previewed, and only the result is sent. The new face shows up right away and is saved to flash (LittleFS), so it survives a reboot. "Remettre d'origine" restores the built-in face.

Test images are in `PROTOfaces/test/`: `mire.png` (calibration chart: red = eye zone, green = nose zone, blue = mouth zone, arrows should point to the middle of the visor), `coeur_0..2.png` and `surpris_0..2.png` (3 blink steps each, select all three at once) and `content.png`.

Upload mode turns itself off after 10 minutes without activity, or when you hold button 3 again. The glove keeps working while it's on.

At boot, the serial monitor (115200 baud) prints free memory after each step (`[mémoire] ...`). If memory runs short, set `cfg.double_buff = false;` in `setupDisplay()`. If the sketch gets too big for flash, pick a partition scheme with more app space, as long as it keeps a SPIFFS/LittleFS partition (e.g. "Minimal SPIFFS").

## Faces

Faces are drawn as 2000x1000 PNGs in `PROTOfaces/` (one panel; the firmware mirrors it on the second one). `tools/png2layers.py` turns them into 1-bit layers (eye / nose / mouth) plus a color gradient per layer, and writes `ProtoHead/faces.h`:

```sh
pip install pillow numpy
python3 tools/png2layers.py --preview preview.png
```

Each face has one PNG per blink step (open → closed). The mouth and the nose are taken from the first one. Full-screen images that shouldn't be split go in `SCREENS` instead. To add a face: draw the PNGs, add them to `FACES` in the script, run it again, then map the face to a button in `BUTTONS`.

Thin lines get lost at 64x32: draw strokes at least ~40 px thick on the 2000x1000 canvas.

## Troubleshooting

- **Picture shifted by one pixel or glitchy**: uncomment `cfg.clkphase = false;` in `setupDisplay()`.
- **Nothing shows up at all**: some panels use an FM6126A driver chip; set `cfg.driver = HUB75_I2S_CFG::FM6126A;`.
- **The head won't boot when the panels are plugged in**: GPIO 12 is a strapping pin, and the shield wires it to the panel's E line.
- **Another protogen using this code nearby**: change `PROTO_ID` in both `protocol.h` files.

## Credits

I would like to give a big thank you to PanickingLynx for his help with the hardware aspect of this project! They did a similar project that inspired me a lot, don't hesitate to check ! 


## Oh ! One more thing !
If anyone uses my code and creates something with it, I would be interested to see ! You can contact me on Discord, Telegram, Twitter, Instagram or any other platforms ! I am always eager to see how others build upon and expand upon my work, and I am happy to provide any support or assistance they may need ! 

#### Thanks for using my code !
