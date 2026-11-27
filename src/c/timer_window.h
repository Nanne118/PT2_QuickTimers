/*
 * timer_window.h - The countdown screen, and all timer logic.
 *
 * This module owns "the" timer: it starts, pauses, extends and cancels it,
 * saves it (via storage.h), schedules the Wakeup that relaunches the app
 * when time is up, and shows the "Done!" alarm.
 *
 * Controls on the countdown screen:
 *   SELECT          pause / resume
 *   SELECT (hold)   add 1 minute
 *   BACK            ask "Cancel timer?"
 *                     -> SELECT: yes, cancel it
 *                     -> BACK:   no, keep it running and leave the app
 * On the "Done!" screen any button dismisses the alarm.
 */
#pragma once

#include <pebble.h>

/*
 * Cookie number attached to our wakeups. When the watch wakes us up, it hands
 * this number back, so main.c can check that the wakeup really is ours.
 */
#define WAKEUP_COOKIE_TIMER 1

/*
 * Start a brand-new countdown of `minutes` minutes and show the timer screen.
 */
void timer_window_start(int minutes);

/*
 * If a timer was saved earlier (the app was closed mid-countdown), reload it
 * and show the timer screen with the correct remaining time. If it already
 * ran out while we were closed, show "Done!" instead.
 * Returns true if a saved timer existed, false if there was nothing to resume.
 */
bool timer_window_resume_saved(void);

/*
 * The timer has finished: show "Done!" and start the vibration alarm.
 * Safe to call more than once (later calls do nothing).
 * Called by main.c when a Wakeup fires.
 */
void timer_window_show_done(void);
