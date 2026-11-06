/* The lowest `render_scale` that still buys anything, as arithmetic.
 *
 * Split from `nr_panel.c` so `test_menu_pacing` can call it without linking ImGui: the panel's own
 * object needs eleven ImGui symbols and two backends, and this is four numbers and a loop.
 *
 * **The rule it implements lives in `src/ref/nr_frame.py`.** The panel runs in the game's process
 * and cannot call Python, so there are two implementations - and `test_menu_pacing` checks this one
 * against values generated from that one, which is the only thing keeping them together.
 */
#ifndef NR_MENU_FLOOR_H
#define NR_MENU_FLOOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Below this the network's field stops shrinking, so every setting down to the knob's old 0.05 ran
 * the same field. `min_extent` is the field's smallest side and is itself a knob, so the answer
 * moves with the picture AND with that. Zero when the size is unknown. */
float nr_panel_lowest_scale(uint32_t width, uint32_t height, uint32_t minimum, float step);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_FLOOR_H */
