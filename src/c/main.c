/*
 * main.c - App entry point: decides which screen to show at launch.
 *
 * Every Pebble app has the same shape:
 *
 *   main() { init(); app_event_loop(); deinit(); }
 *
 * init() sets things up, app_event_loop() hands control to Pebble (which
 * then calls our callbacks for buttons, ticks, drawing, ...) until the user
 * leaves the app, and deinit() cleans up.
 *
 * At launch there are three cases:
 *   1. The watch woke us up because a timer ended  -> show "Done!".
 *   2. A timer is still running from a previous visit -> show the countdown.
 *   3. Nothing going on                               -> just the grid.
 * The grid is always pushed first, so it's underneath the timer screen and
 * BACK/dismiss returns to it.
 */
#include <pebble.h>
#include "grid_window.h"
#include "timer_window.h"

/*
 * WakeupService callback: Pebble calls this if one of our wakeups fires
 * while the app is ALREADY open (otherwise it launches the app instead,
 * which is case 1 in init()).
 *   wakeup_id: which wakeup fired.
 *   cookie:    the number we passed to wakeup_schedule().
 */
static void handle_wakeup(WakeupId wakeup_id, int32_t cookie) {
  if (cookie == WAKEUP_COOKIE_TIMER) {
    timer_window_show_done();
  }
}

/*
 * Was this launch caused by our timer's wakeup?
 * launch_reason() says why the app started; wakeup_get_launch_event() fills
 * in the id and cookie of the wakeup (we pass addresses with & so it can
 * write into our local variables).
 */
static bool launched_by_timer_wakeup(void) {
  if (launch_reason() != APP_LAUNCH_WAKEUP) {
    return false;
  }
  WakeupId id = 0;
  int32_t cookie = 0;
  wakeup_get_launch_event(&id, &cookie);
  return cookie == WAKEUP_COOKIE_TIMER;
}

/* Set up the app (see the three cases at the top of this file). */
static void init(void) {
  grid_window_push();
  wakeup_service_subscribe(handle_wakeup);

  if (launched_by_timer_wakeup()) {
    timer_window_show_done();
  } else {
    timer_window_resume_saved();  // Does nothing if no timer was saved.
  }
}

/*
 * Clean up before the app exits. Popping every window runs each window's
 * unload handler, which is where the layers and windows are freed (see
 * grid_window.c and timer_window.c). The timer itself is already saved, so
 * nothing is lost.
 */
static void deinit(void) {
  window_stack_pop_all(false);
}

int main(void) {
  init();
  app_event_loop();  // Blocks until the user leaves the app.
  deinit();
}
