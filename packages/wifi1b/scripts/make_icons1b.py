#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Wifi1b tile glyph: the blue WiFi arcs plus a small '2' badge."""
from PIL import Image, ImageDraw
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "artifacts", "assets", "wifi1b")


def make(color: str, path: str, badge: bool) -> None:
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((44, 66, 55, 77), fill=color)
    d.arc((27, 48, 72, 93), 225, 315, fill=color, width=7)
    d.arc((17, 33, 82, 98), 225, 315, fill=color, width=7)
    d.arc((7, 18, 92, 103), 225, 315, fill=color, width=7)
    if badge:
        d.ellipse((63, 63, 95, 95), fill="#2F6FED", outline="#FFFFFF", width=2)
        d.text((74, 68), "2", fill="#FFFFFF")
    im.save(path)


os.makedirs(OUT, exist_ok=True)
make("#2F6FED", os.path.join(OUT, "wifi1b_icon_n.png"), True)
make("#8A8A8A", os.path.join(OUT, "wifi1b_icon_d.png"), False)
print("wrote", OUT)
