/*
 * storage.h - Saving and loading the active timer.
 *
 * WHY THIS FILE EXISTS
 * A Pebble app is killed as soon as you leave it, and every variable is lost.
 * To let a timer "keep running" while the app is closed, we save the moment
 * the timer will END (an absolute timestamp) to persistent storage. When the
 * app starts again we read it back and compute:
 *
 *     remaining = end_time - now
 *
 * Saving a counter ("427 seconds left") would not work: nothing would count it
 * down while the app is closed. A timestamp stays correct however long we're
 * away.
 *
 * Other files only see the functions below; *how* the data is stored
 * (keys, versions) stays hidden in storage.c.
 */
#pragma once

#include <pebble.h>

/*
 * Everything we need to rebuild a timer after the app was closed.
 *
 * time_t is a number of seconds since 1 Jan 1970 (a "Unix timestamp").
 * WakeupId is the Pebble SDK's handle for a scheduled wakeup (a signed
 * 32-bit integer; negative values mean "none / error").
 */
typedef struct {
  time_t end_time;           // When running: the moment the timer reaches 0.
  int32_t total_seconds;     // Full length incl. added minutes (for the ring).
  int32_t paused_remaining;  // When paused: seconds that were left.
  bool is_paused;            // true = frozen, end_time is not meaningful.
  WakeupId wakeup_id;        // Scheduled wakeup, or -1 if none.
} TimerState;

/*
 * Write the timer state to persistent storage.
 *   state: pointer to the state to save. We take a pointer instead of a copy
 *          so the struct isn't copied onto the (small) stack, and `const`
 *          promises we won't modify the caller's data.
 */
void storage_save_timer(const TimerState *state);

/*
 * Read the saved timer state.
 *   out: pointer to a TimerState that this function fills in. The caller
 *        owns the memory (usually a local or static variable); we just
 *        write into it.
 * Returns true if a valid saved timer existed, false otherwise
 * (in which case *out is left untouched).
 */
bool storage_load_timer(TimerState *out);

/*
 * Delete the saved timer, e.g. after it finished or was cancelled, so the
 * next app launch shows the grid instead of a stale timer.
 */
void storage_clear_timer(void);
