/*
 * grid_window.h - The main menu: a 2x2 grid of timer buttons (5/10/15/30 min).
 *
 * Controls:
 *   UP / DOWN  move the highlight through the four squares (wraps around)
 *   SELECT     start the highlighted timer
 *   Touch      tap a square to start it (Pebble Time 2 only; see PBL_TOUCH)
 */
#pragma once

#include <pebble.h>

/*
 * Create the grid window and push it onto the window stack (make it visible).
 * The window destroys itself when it is popped (see grid_window.c, unload).
 */
void grid_window_push(void);
