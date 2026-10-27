#!/usr/bin/env python3
"""A page of every built-in knob look in a plugin's colours, to pick one:  knob_gallery.py <Plugin> [out.html]

Draws each look (mpc-vst-plugins tools/html_art.py, the browser renderer's own drawing and default.css) at three
positions on the plugin's theme (its layout.conf theme_ lines), next to the current dotted-ring knob from the
built skin. Open the page in a browser; nothing is built or changed.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MPC = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(MPC, "mpc-vst-plugins", "tools"))
import html_art  # noqa: E402


ARCS = (("taper",), (("orange on dark", "ff7a1a", "2e2e2e"), ("amber on dark", "ffb000", "2e2e2e"),
                     ("hot orange-red on dark", "ff4a1a", "2e2e2e"), ("orange on brown", "ff7a1a", "4a2a14"),
                     ("S950 green on dark", "7dff6a", "2e2e2e"), ("cyan on dark", "3ad6ff", "2e2e2e"),
                     ("white on dark", "f2f2f2", "2e2e2e"), ("current theme", "f2f2f2", "ff7a1a")))


def main(plugin, out, arcs=ARCS):
    pdir = os.path.join(MPC, plugin)
    cfg = json.load(open(os.path.join(pdir, "vst.json")))
    art = html_art.Art()
    art.load_theme(os.path.join(pdir, cfg["layout"]))
    r, s = 48, 2 * 48 + 10
    vars_ = ";".join("--%s:%s" % (k.replace("_", "-"), html_art.hexc(v)) for k, v in art.theme.items())
    css = os.path.join(MPC, "mpc-vst-plugins", "tools", "html_art", "default.css")
    font = os.path.join(MPC, "mpc-vst-plugins", "tools", "html_art", "fonts", "TitilliumWeb-SemiBold.ttf")

    def cell(name, svgs):
        return ('<div class="cell"><div class="row">%s</div><div class="name">%s</div></div>' % (
            "".join(svgs), name))

    def svg(body):
        return '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">%s%s</svg>' % (s, s, html_art.DEFS, body)

    cells = []
    strip = os.path.join(pdir, "build", "skin")
    cur = []
    for root, _, files in os.walk(strip):
        if "sh_knob_r%d.png" % r in files:
            cur = [os.path.join(root, "sh_knob_r%d.png" % r)]
    if cur:
        # the current knob: frames 0, 64, 127 of the built filmstrip (s x s each, stacked down)
        imgs = ['<div class="strip" style="width:%dpx;height:%dpx;background:url(\'file://%s\') 0 -%dpx"></div>'
                % (s, s, cur[0], f * s) for f in (0, 64, 127)]
        cells.append(cell("current (dotted ring)", imgs))
    cells.append(cell("drawn (browser default)", [svg(art.knob_svg(s / 2, s / 2, r, p)) for p in (0, 50, 100)]))
    for look in html_art.KNOB_LOOKS:
        cells.append(cell(look, [svg(art.knob_frame(s / 2, s / 2, r, p, {"look": look})) for p in (0, 50, 100)]))
    # arc colours for the arc looks: (value arc = theme_knob_dot, track = theme_knob_ring)
    if arcs:
        for look in arcs[0]:
            for name, arc, track in arcs[1]:
                st = ' style="--knob-dot:#%s;--knob-ring:#%s"' % (arc, track)
                cells.append(cell("%s: %s<br><small>knob_dot=%s knob_ring=%s</small>" % (look, name, arc, track),
                                  ['<div%s>%s</div>' % (st, svg(art.knob_frame(s / 2, s / 2, r, p, {"look": look})))
                                   for p in (0, 30, 75)]))

    page = ('<!doctype html><html><head><meta charset="utf-8"><title>%s knobs</title>'
            '<link rel="stylesheet" href="file://%s"><style>'
            '@font-face{font-family:"Titillium Web";src:url("file://%s")}'
            ':root{%s}body{margin:24px;background:var(--bg);color:var(--ink);font:600 18px "Titillium Web",sans-serif}'
            'h1{font-size:26px;color:var(--accent-hi);margin:0 0 16px}'
            '.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(360px,1fr));gap:16px}'
            '.cell{background:var(--panel);border:1px solid var(--line);padding:12px}'
            '.row{display:flex;gap:6px;justify-content:center}.name{text-align:center;margin-top:8px;font-size:20px}'
            '</style></head><body><h1>%s: knob looks (0%%, 50%%, 100%%)</h1><div class="grid">%s</div></body></html>') % (
        plugin, css, font, vars_, plugin, "".join(cells))
    open(out, "w").write(page)
    print(out)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(MPC, sys.argv[1], "build", "knobs.html"))
