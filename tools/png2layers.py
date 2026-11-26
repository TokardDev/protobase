#!/usr/bin/env python3
"""
Convertit les PNG de PROTOfaces/ en calques 1 bit pour le firmware de la tête.

Chaque PNG (2000x1000, ratio 2:1) représente UN panneau 64x32 ; le firmware
l'affiche en miroir sur le second. L'image est découpée en 3 calques :
  - l'oeil   : moitié haute, hors zone du nez  (une image par étape de clignement)
  - le nez   : moitié haute, colonnes de droite
  - la bouche: moitié basse
La couleur n'est pas stockée pixel par pixel : on extrait un dégradé horizontal
(couleur à gauche -> couleur à droite) par calque, appliqué à l'affichage.

Usage :
    pip install pillow numpy
    python3 tools/png2layers.py                      # génère ProtoHead/faces.h
    python3 tools/png2layers.py --preview apercu.png # + un aperçu agrandi

Pour ajouter une face : dessine-la en PNG 2000x1000 (même cadrage que les
autres), ajoute-la dans FACES ci-dessous, relance le script.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "PROTOfaces"
OUT = ROOT / "ProtoHead" / "faces.h"

W, H = 64, 32          # un panneau
SPLIT_Y = 16           # lignes < SPLIT_Y : oeil + nez ; >= SPLIT_Y : bouche
NOSE_X = 56            # dans la moitié haute, colonnes >= NOSE_X : nez
LUM_MIN = 60           # luminosité min (0-255) pour qu'un pixel source compte
COVERAGE = 0.12        # part min d'une cellule 64x32 allumée pour allumer la LED
# ⚠ Ces réglages (et SPLIT_Y / NOSE_X) sont dupliqués dans le convertisseur JavaScript
#   de la page d'upload (ProtoHead/webui.h) : garde-les identiques.

# nom de la face -> étapes de clignement, de "oeil ouvert" à "oeil fermé".
# La bouche et le nez sont pris sur la première image.
FACES = {
    "neutral": ["clin0.png", "clin1.png", "clin2.png"],
    "angry":   ["Fache.png", "Fache1.png", "Fache2.png"],
    "uwu":     ["uwu0.png"],    # >w< : yeux déjà plissés, donc une seule image (pas de clignement)
    "happy":   ["happy0.png", "happy1.png", "happy2.png"],  # cyan : oeil neutre retourné à la verticale, bouche ondulée
}

# Écrans pleins : l'image entière devient un seul calque <nom>_screen, sans découpage.
SCREENS = {
    # "nom": "fichier.png",
}


def load(path):
    a = np.asarray(Image.open(path).convert("RGBA")).astype(np.float32)
    lit = a[..., :3].max(-1) * a[..., 3] / 255 > LUM_MIN
    rgb = a[..., :3]
    # chaque pixel source tombe dans une cellule 64x32 ; on compte les pixels allumés
    # par cellule (couverture) et on fait la moyenne de leurs couleurs
    h, w = lit.shape
    sums = np.zeros((H, W, 3))
    counts = np.zeros((H, W))
    ys, xs = np.nonzero(lit)
    cy = ys * H // h
    cx = xs * W // w
    np.add.at(sums, (cy, cx), rgb[ys, xs])
    np.add.at(counts, (cy, cx), 1)
    mask = counts / ((w / W) * (h / H)) > COVERAGE
    colors = sums / np.maximum(counts, 1)[..., None]
    return mask, colors


def region(kind):
    r = np.zeros((H, W), bool)
    if kind == "eye":
        r[:SPLIT_Y, :NOSE_X] = True
    elif kind == "nose":
        r[:SPLIT_Y, NOSE_X:] = True
    elif kind == "full":
        r[:, :] = True
    else:
        r[SPLIT_Y:, :] = True
    return r


def make_layer(mask, colors, kind):
    m = mask & region(kind)
    cols = np.nonzero(m.any(0))[0]
    if len(cols) == 0:
        return m, (0, 0), ((0, 0, 0), (0, 0, 0))
    x0, x1 = int(cols[0]), int(cols[-1])
    # dégradé : moyenne des couleurs sur le premier / dernier quart de la largeur
    span = max(1, (x1 - x0 + 1) // 4)
    def avg(lo, hi):
        sel = m.copy()
        sel[:, :lo] = False
        sel[:, hi:] = False
        c = colors[sel].mean(0)
        return tuple(int(np.floor(v + 0.5)) for v in c)  # comme Math.round en JS
    return m, (x0, x1), (avg(x0, x0 + span), avg(x1 - span + 1, x1 + 1))


def pack(m):
    """64x32 -> 256 octets, ligne par ligne, MSB = pixel de gauche."""
    return np.packbits(m.astype(np.uint8), axis=1).flatten()


def c_layer(name, layer):
    m, (x0, x1), (c0, c1) = layer
    data = pack(m)
    lines = []
    for y in range(H):
        row = data[y * 8:(y + 1) * 8]
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in row) + ",")
    return (
        f"static const uint8_t {name}_bits[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"
        f"static const Layer {name} = {{ {name}_bits, {x0}, {x1}, "
        f"{{{c0[0]}, {c0[1]}, {c0[2]}}}, {{{c1[0]}, {c1[1]}, {c1[2]}}} }};\n"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="écrit un aperçu PNG agrandi des calques")
    args = ap.parse_args()

    out = [
        "// GÉNÉRÉ AUTOMATIQUEMENT par tools/png2layers.py — ne pas éditer à la main.",
        "// Pour modifier une face : change le PNG dans PROTOfaces/ et relance le script.",
        "#pragma once",
        "#include \"layers.h\"",
        "",
    ]
    preview = []
    face_defs = []
    for face, frames in FACES.items():
        eyes = []
        for i, f in enumerate(frames):
            mask, colors = load(SRC / f)
            name = f"{face}_eye{i}"
            layer = make_layer(mask, colors, "eye")
            out.append(c_layer(name, layer))
            eyes.append((name, layer))
            if i == 0:
                mouth = make_layer(mask, colors, "mouth")
                nose = make_layer(mask, colors, "nose")
                out.append(c_layer(f"{face}_mouth", mouth))
                out.append(c_layer(f"{face}_nose", nose))
        face_defs.append(
            f"  {{ {{ {', '.join('&' + n for n, _ in eyes)} }}, {len(eyes)}, "
            f"&{face}_mouth, &{face}_nose }},  // {face}"
        )
        for _, eye in eyes:
            preview.append([eye, mouth, nose])

    for screen, f in SCREENS.items():
        mask, colors = load(SRC / f)
        layer = make_layer(mask, colors, "full")
        out.append(c_layer(f"{screen}_screen", layer))
        preview.append([layer])

    out.append("enum FaceId : uint8_t {")
    for i, face in enumerate(FACES):
        out.append(f"  FACE_{face.upper()} = {i},")
    out.append(f"  FACE_COUNT = {len(FACES)}")
    out.append("};\n")
    out.append("static const char *const FACE_NAMES[FACE_COUNT] = {")
    out.append("  " + ", ".join(f'"{face}"' for face in FACES))
    out.append("};\n")
    out.append("static const FaceDef FACES[FACE_COUNT] = {")
    out.extend(face_defs)
    out.append("};")
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"écrit {OUT.relative_to(ROOT)}")

    if args.preview:
        S = 8
        img = np.zeros((len(preview) * (H + 2) * S, W * 2 * S, 3), np.uint8)
        for k, layers in enumerate(preview):
            for m, (x0, x1), (c0, c1) in layers:
                for y, x in zip(*np.nonzero(m)):
                    t = (x - x0) / max(1, x1 - x0)
                    c = [int(a + (b - a) * t) for a, b in zip(c0, c1)]
                    oy = k * (H + 2) + y
                    for xx in (x, 2 * W - 1 - x):  # miroir comme sur la visière
                        img[oy * S:(oy + 1) * S - 1, xx * S:(xx + 1) * S - 1] = c
        Image.fromarray(img).save(args.preview)
        print(f"aperçu : {args.preview}")


if __name__ == "__main__":
    main()
