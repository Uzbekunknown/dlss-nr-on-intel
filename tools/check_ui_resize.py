"""Does the panel's image follow the swapchain's extent?

The bug this pins: the UI image was created once at 640x360 and never resized, while
`nrex_new_frame` was told the swapchain's current extent every frame. After a resolution change -
or a `render_scale` change that recreates the swapchain - ImGui laid the panel out for the new size
and drew it into an image of the old one, so everything past 640x360 was clipped away.

`nrex_resize` existed and nothing called it, which is a whole class of bug this project has already
paid for twice: the menu that compiled but was never built, and the destroy that only the tests
called. So the call is checked here rather than reasoned about.

Run: python check_ui_resize.py
"""
import io
import pathlib
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')

REPO = pathlib.Path(__file__).resolve().parent.parent
LAYER = REPO / 'src' / 'layer' / 'nr_layer.c'
UI_H = REPO / 'src' / 'layer' / 'nr_menu_ui.h'
UI_CPP = REPO / 'src' / 'layer' / 'nr_menu_ui.cpp'

fails = 0


def check(name, ok, detail=''):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{('  ' + detail) if detail else ''}")
    if not ok:
        fails += 1


layer = io.open(LAYER, encoding='utf-8').read()
header = io.open(UI_H, encoding='utf-8').read()
ui = io.open(UI_CPP, encoding='utf-8').read()

print('test_ui_resize: does the panel follow the swapchain?\n')

# 1. The interface exists.
check('nrex_resize is declared', 'nrex_resize' in header)

# 2. It really replaces the target rather than only recording a size.
m = re.search(r'nrex_resize\s*\([^)]*\)\s*\{(.*?)\n\}', ui, re.S)
body = m.group(1) if m else ''
check('nrex_resize drops the old target', 'nrex_drop_target' in body)
check('nrex_resize makes a new one', 'nrex_make_target' in body)

# 3. The layer calls it, from the composite path.
check('the layer calls nrex_resize', 'nrex_resize(' in layer)

# 4. And compares the extent before doing so, rather than resizing every frame - which would
#    destroy and recreate a GPU image 60 times a second.
check('the layer compares width and height first',
      'nrex_image_width(data->menu_ui) != chain->extent.width' in layer
      and 'nrex_image_height(data->menu_ui) != chain->extent.height' in layer)

# 5. The previous frame is waited for before the image is destroyed. Destroying an image the GPU is
#    still reading is undefined, and the frame before this one both drew into it and sampled it.
resize_at = layer.index('nrex_resize(')
before = layer[max(0, resize_at - 1200):resize_at]
check('the previous frame is waited for before the resize',
      'WaitForFences' in before)

# 6. The new frame is told the same extent the image was made for - the disagreement was the bug.
check('nrex_new_frame is told the swapchain extent',
      'nrex_new_frame(data->menu_ui, (float)chain->extent.width' in layer)

# 7. The panel's own creation size is a starting value, not the only one.
check('the UI is created at a default that the resize can replace',
      'nrex_make_target(u, 640, 360' in ui)

print()
print('ok' if not fails else f'FAILED ({fails})')
raise SystemExit(1 if fails else 0)
