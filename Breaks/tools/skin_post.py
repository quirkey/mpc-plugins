#!/usr/bin/env python3
"""vst.json "skin_post": run by gen_vst.py on the built skin folder:  skin_post.py <skin dir>

Knob names are MPC's own Name labels (the parameter's name, drawn live); shadow_skin.py writes them in the name's
own case. Breaks shows them in capitals, like its titles and buttons, so set every Name label to "Upper Case".

The PADS tiles hold their effect while touched: see pad_tiles_hold().
"""
import copy
import json
import os
import struct
import sys
import zlib


def upcase(node):
    n = 0
    if isinstance(node, dict):
        if node.get("type") == "Name" and isinstance(node.get("textStyle"), dict):
            node["textStyle"]["case"] = "Upper Case"
            n += 1
        for v in node.values():
            n += upcase(v)
    elif isinstance(node, list):
        for v in node:
            n += upcase(v)
    return n


def hit_png(path, w, h):
    """A plate of alpha 1: a hit, not seen (as shadow_skin.py's write_hit_png)."""
    raw = b"".join(b"\0" + b"\0\0\0\1" * w for _ in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def pad_tiles_hold(tui, skin):
    """A list row's live text lies over its button and takes the touch, so MPC acted on the finger going down only
    (a tile latched) and on the lift only when a touch missed the text. A see-through button on top of the text, as
    shadow_skin.py gives a trigger button with a live caption, gets the whole touch, down and up, like PREV / NEXT:
    a tile then holds its effect while touched (Breaks.cpp counts the touches)."""
    n = 0
    for comp in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
        if not comp.get("key", "").startswith("shRow_"):
            continue
        parts = comp["value"]["componentsData"]
        buttons = [c for c in parts if c["componentData"]["type"] == "Button"]
        if len(buttons) != 1:
            continue
        w, h = (int(v) for v in buttons[0]["bounds"]["bounds"].split()[2:4])
        name = "sh_pad_hit_%dx%d.png" % (w, h)
        hit_png(os.path.join(skin, "Plugin Skins", name), w, h)
        plate = copy.deepcopy(buttons[0])
        plate["componentData"]["data"]["onImage"] = plate["componentData"]["data"]["offImage"] = name
        parts.append(plate)
        n += 1
    return n


def main(skin):
    path = os.path.join(skin, "Plugin Skins", "TUI.json")
    tui = json.load(open(path))
    n = upcase(tui)
    if not n:
        sys.exit("skin_post: no Name labels found in %s" % path)
    t = pad_tiles_hold(tui, skin)
    if not t:
        sys.exit("skin_post: no PADS tiles found in %s" % path)
    with open(path, "w") as f:
        json.dump(tui, f, indent=2)
    print("skin_post: %d knob names in capitals, %d tile look(s) hold while touched" % (n, t))


if __name__ == "__main__":
    main(sys.argv[1])
