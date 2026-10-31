/* Raw mouse movement, for a panel that pins the system cursor.
 *
 * A game in a relative-input mode re-centres the cursor whenever it likes, so the panel pins it:
 * ClipCursor to a one-pixel box. Once pinned, GetCursorPos cannot report movement - it returns the
 * box - so the movement has to come from somewhere else. It arrives as raw input, which is
 * independent of where the cursor is being held.
 *
 * Why a window of our own. Raw input is delivered to a registered window and the layer has none:
 * it runs on the game render thread and never takes part in its message loop. So it makes a hidden
 * one and registers with RIDEV_INPUTSINK, which asks for the input even when that window is not
 * foreground. That is exclusive, not broadcast: a process holds one registration per device class,
 * so ours replaces the game's while the panel has the pointer, and the game's is put back after.
 *
 * Why polling, not a message loop. GetRawInputBuffer drains whatever is queued without a WndProc,
 * which is the only shape available to a caller that must not pump the game messages. Called once
 * per frame, it turns one frame of movement into one frame of delta.
 */
#ifndef NR_RAW_MOUSE_H
#define NR_RAW_MOUSE_H

/* Start receiving raw mouse movement, or stop. Returns 1 when a device was registered.
 *
 * The keyboard's registration is taken with the mouse's and given back with it: while it is ours,
 * a game that reads its keys as raw input is not handed the ones typed into the panel. The game's
 * own registrations, target window and flags, are what `stop` puts back. */
int nr_raw_mouse_start(void);
void nr_raw_mouse_stop(void);

/* The movement accumulated since the last call, in device units - the same units a game mouse look
 * uses. Returns 1 when something moved. */
int nr_raw_mouse_take(int *dx, int *dy);

#endif /* NR_RAW_MOUSE_H */
