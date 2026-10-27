#!/usr/bin/env python3
"""vst.json "skin_post": run by gen_vst.py on the built skin folder:  skin_post.py <skin dir>

Knob names are MPC's own Name labels (the parameter's name, drawn live); shadow_skin.py writes them in the name's
own case. SamplerFilter shows them in capitals, like Breaks, so set every Name label to "Upper Case".
"""
import json
import os
import sys


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


def main(skin):
    path = os.path.join(skin, "Plugin Skins", "TUI.json")
    tui = json.load(open(path))
    n = upcase(tui)
    if not n:
        sys.exit("skin_post: no Name labels found in %s" % path)
    with open(path, "w") as f:
        json.dump(tui, f, indent=2)
    print("skin_post: %d knob names in capitals" % n)


if __name__ == "__main__":
    main(sys.argv[1])
