/*
 * timer_window.c - Countdown screen, timer logic and the "Done!" alarm.
 *
 * THE BIG IDEA: TIME IS A TIMESTAMP, NOT A COUNTER
 * We never count down a variable. We store the moment the timer ends
 * (s_state.end_time) and every second compute `end_time - now`. That's what
 * makes the countdown correct after the app is closed and reopened.
 *
 * WHAT KEEPS THE TIMER ALIVE WHEN THE APP IS CLOSED?
 * The Wakeup API. wakeup_schedule() asks the watch's OS to relaunch this
 * app at a given time, even if the user is on the watchface or in another
 * app. main.c notices the wakeup launch and calls timer_window_show_done().
 *
 * MODES
 * The window is always in one of three modes (see TimerMode below). Each
 * mode decides what's on screen and what the buttons do.
 */
#include "timer_window.h"
#include "storage.h"

// ---- Timing constants -------------------------------------------------------
// (SECONDS_PER_MINUTE comes from pebble.h, so we don't define it ourselves;
//  redefining an existing macro is an error with the SDK's -Werror flags.)
#define ADD_TIME_SECONDS          60     // Long-press SELECT adds this much.
#define LONG_PRESS_MS             500    // How long SELECT must be held.
#define DONE_VIBE_INTERVAL_MS     10000  // Repeat the alarm every 10 s...
#define DONE_VIBE_MAX_REPEATS     6      // ...6 times (0,10,..,50 s) = max 1 minute.
#define NO_WAKEUP                 (-1)   // Our "no wakeup scheduled" marker.

// ---- Layout constants (pixels) ----------------------------------------------
#define RING_DIAMETER             150
#define RING_TOP                  14
#define RING_THICKNESS            14
#define TIME_TEXT_HEIGHT          40
#define STATUS_TOP                172
#define STATUS_HEIGHT             52
#define DONE_TEXT_HEIGHT          50
#define DONE_HINT_HEIGHT          30
// Room for "MM:SS" and '\0'. 16, not 6: the compiler checks the worst case of
// "%02d:%02d" with any int (14 bytes) and refuses to build if it might not fit.
#define TIME_TEXT_BUFFER_SIZE     16

// ---- Colors -----------------------------------------------------------------
#define COLOR_TIMER_BACKGROUND    GColorBlack
#define COLOR_DONE_BACKGROUND     GColorIslamicGreen
#define COLOR_RING_TRACK          GColorDarkGray   // The "empty" part of the ring.
#define COLOR_RING_RUNNING        GColorChromeYellow
#define COLOR_RING_PAUSED         GColorLightGray
#define COLOR_TEXT                GColorWhite
#define COLOR_CONFIRM_TEXT        GColorRajah      // Makes the cancel prompt stand out.

// ---- Text -------------------------------------------------------------------
#define TEXT_RUNNING   "SELECT: pause\nHold SELECT: +1 min"
#define TEXT_PAUSED    "Paused\nSELECT: resume"
#define TEXT_CONFIRM   "Cancel timer?\nSELECT: yes  BACK: keep"
#define TEXT_DONE      "Done!"
#define TEXT_DONE_HINT "Press any button"

/*
 * What the screen is currently doing. An enum gives names to small integers,
 * which reads much better than "if (mode == 2)".
 */
typedef enum {
  MODE_COUNTING,        // Running or paused (see s_state.is_paused).
  MODE_CONFIRM_CANCEL,  // "Cancel timer?" prompt; timer keeps counting.
  MODE_DONE,            // Time is up, alarm showing.
} TimerMode;

/*
 * The alarm vibration: on/off durations in milliseconds, starting with "on".
 * Two short buzzes and a long one, so it feels different from a notification.
 */
static const uint32_t DONE_VIBE_SEGMENTS[] = { 200, 100, 200, 100, 600 };

// ---- State ------------------------------------------------------------------
// Heap objects created via the SDK (each needs a matching *_destroy()):
static Window *s_window;
static Layer *s_ring_layer;
static TextLayer *s_time_layer;
static TextLayer *s_status_layer;
static TextLayer *s_done_layer;
static TextLayer *s_done_hint_layer;
// Handle to the pending repeat-vibration timer, or NULL if none. Pebble
// frees an AppTimer itself once it fires or is cancelled.
static AppTimer *s_vibe_timer;

// The timer itself (also what we save). Statics start zeroed, but 0 is a
// valid WakeupId, so set the "none" marker explicitly.
static TimerState s_state = { .wakeup_id = NO_WAKEUP };
static TimerMode s_mode = MODE_COUNTING;
static int s_vibe_count;     // Alarm buzzes so far.

// TextLayer does NOT copy its text: it keeps the pointer we give it. So the
// buffer must outlive the layer, which is why it's static, not a local.
static char s_time_text[TIME_TEXT_BUFFER_SIZE];

// Forward declaration: defined further down, but used before that.
static void handle_timer_done(void);

// =============================================================================
// Timer logic (no drawing here)
// =============================================================================

/*
 * Seconds left on the timer, never negative.
 * When paused we use the frozen value; when running we compute it from the
 * end timestamp, which is what keeps it correct across app restarts.
 */
static int32_t get_remaining_seconds(void) {
  if (s_state.is_paused) {
    return s_state.paused_remaining;
  }
  int32_t remaining = (int32_t)(s_state.end_time - time(NULL));
  return remaining > 0 ? remaining : 0;
}

/*
 * Turn seconds into "MM:SS".
 *   seconds:     value to format (e.g. 125).
 *   buffer:      where to write the text (e.g. "02:05"). A pointer to the
 *                first char of an array owned by the caller.
 *   buffer_size: size of that array, so snprintf can never write past its
 *                end (a "buffer overflow", a classic C bug).
 */
static void format_time(int32_t seconds, char *buffer, size_t buffer_size) {
  int minutes = seconds / SECONDS_PER_MINUTE;
  int secs = seconds % SECONDS_PER_MINUTE;
  snprintf(buffer, buffer_size, "%02d:%02d", minutes, secs);
}

/*
 * Ask the OS to relaunch the app at s_state.end_time, replacing any
 * wakeup we had scheduled before. Stores the new id in s_state.
 *
 * wakeup_schedule() can fail, e.g. with E_RANGE if any app already has a
 * wakeup within one minute of that time. We log it and carry on: the
 * countdown still works while the app is open, it just can't relaunch us.
 */
static void schedule_wakeup(void) {
  if (s_state.wakeup_id != NO_WAKEUP) {
    wakeup_cancel(s_state.wakeup_id);
  }
  // notify_if_missed = true: if the watch was off at that moment, the OS
  // shows a notification later so the alarm isn't silently lost.
  WakeupId id = wakeup_schedule(s_state.end_time, WAKEUP_COOKIE_TIMER, true);
  if (id < 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed: %d", (int)id);
    s_state.wakeup_id = NO_WAKEUP;
  } else {
    s_state.wakeup_id = id;
  }
}

/* Cancel our scheduled wakeup, if any. */
static void cancel_wakeup(void) {
  if (s_state.wakeup_id != NO_WAKEUP) {
    wakeup_cancel(s_state.wakeup_id);
    s_state.wakeup_id = NO_WAKEUP;
  }
}

/*
 * Freeze the timer: remember how much was left and cancel the wakeup
 * (a paused timer must not ring).
 */
static void pause_timer(void) {
  s_state.paused_remaining = get_remaining_seconds();
  s_state.is_paused = true;
  cancel_wakeup();
  storage_save_timer(&s_state);  // &s_state = address of our struct.
}

/*
 * Unfreeze: the new end is "now + what was left". Then reschedule the wakeup.
 */
static void resume_timer(void) {
  s_state.end_time = time(NULL) + s_state.paused_remaining;
  s_state.is_paused = false;
  s_state.paused_remaining = 0;
  schedule_wakeup();
  storage_save_timer(&s_state);
}

/*
 * Add ADD_TIME_SECONDS to the timer, running or paused. total_seconds grows
 * too, so the ring keeps showing the fraction of the whole time remaining.
 */
static void add_time(void) {
  if (s_state.is_paused) {
    s_state.paused_remaining += ADD_TIME_SECONDS;
  } else {
    s_state.end_time += ADD_TIME_SECONDS;
    schedule_wakeup();  // The old wakeup would ring a minute too early.
  }
  s_state.total_seconds += ADD_TIME_SECONDS;
  storage_save_timer(&s_state);
}

/*
 * Stop the timer for good: no wakeup, nothing saved.
 */
static void stop_timer(void) {
  cancel_wakeup();
  storage_clear_timer();
}

// =============================================================================
// Drawing
// =============================================================================

/*
 * The ring layer's update procedure (a Pebble callback, like draw_grid()).
 *   layer: s_ring_layer.
 *   ctx:   graphics context to draw with.
 * Draws a gray full circle (the track), then a colored arc on top whose
 * length is proportional to the time remaining, so it shrinks clockwise.
 */
static void draw_progress_ring(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Track: a full circle from angle 0 to TRIG_MAX_ANGLE (= 360 degrees).
  graphics_context_set_fill_color(ctx, COLOR_RING_TRACK);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, RING_THICKNESS,
                       0, TRIG_MAX_ANGLE);

  if (s_state.total_seconds <= 0) {
    return;  // Avoid dividing by zero below.
  }

  // Fraction left, as an angle. The multiplication is done in 64 bits:
  // TRIG_MAX_ANGLE is 65536, and 65536 * (a long timer) would overflow a
  // 32-bit int.
  int32_t angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * get_remaining_seconds()
                            / s_state.total_seconds);

  graphics_context_set_fill_color(ctx, s_state.is_paused ? COLOR_RING_PAUSED : COLOR_RING_RUNNING);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, RING_THICKNESS, 0, angle);
}

/*
 * Bring every layer in line with the current state: which layers are
 * visible, what the text says, and redraw the ring. Called after anything
 * changes and once per second.
 */
static void update_display(void) {
  bool done = (s_mode == MODE_DONE);

  // Show either the countdown layers or the "Done!" layers.
  layer_set_hidden(s_ring_layer, done);
  layer_set_hidden(text_layer_get_layer(s_time_layer), done);
  layer_set_hidden(text_layer_get_layer(s_status_layer), done);
  layer_set_hidden(text_layer_get_layer(s_done_layer), !done);
  layer_set_hidden(text_layer_get_layer(s_done_hint_layer), !done);
  window_set_background_color(s_window, done ? COLOR_DONE_BACKGROUND : COLOR_TIMER_BACKGROUND);

  if (done) {
    return;
  }

  format_time(get_remaining_seconds(), s_time_text, sizeof(s_time_text));
  // The layer only stores the pointer, so it doesn't notice the buffer
  // changed. Setting the text again marks it dirty so it redraws.
  text_layer_set_text(s_time_layer, s_time_text);

  if (s_mode == MODE_CONFIRM_CANCEL) {
    text_layer_set_text(s_status_layer, TEXT_CONFIRM);
    text_layer_set_text_color(s_status_layer, COLOR_CONFIRM_TEXT);
  } else {
    text_layer_set_text(s_status_layer, s_state.is_paused ? TEXT_PAUSED : TEXT_RUNNING);
    text_layer_set_text_color(s_status_layer, COLOR_TEXT);
  }

  layer_mark_dirty(s_ring_layer);
}

// =============================================================================
// The alarm
// =============================================================================

/* Play the alarm pattern once. */
static void play_done_vibe(void) {
  VibePattern pattern = {
    .durations = DONE_VIBE_SEGMENTS,
    .num_segments = ARRAY_LENGTH(DONE_VIBE_SEGMENTS),  // SDK macro: element count.
  };
  vibes_enqueue_custom_pattern(pattern);
  light_enable_interaction();  // Turn the backlight on briefly, like a button press.
}

/*
 * AppTimer callback: Pebble calls this DONE_VIBE_INTERVAL_MS after we
 * registered it.
 *   context: the pointer passed to app_timer_register() (unused).
 * Buzzes again and re-registers itself until DONE_VIBE_MAX_REPEATS is reached.
 */
static void vibe_timer_callback(void *context) {
  s_vibe_timer = NULL;  // This timer has fired; Pebble has already freed it.
  play_done_vibe();
  s_vibe_count++;
  if (s_vibe_count < DONE_VIBE_MAX_REPEATS) {
    s_vibe_timer = app_timer_register(DONE_VIBE_INTERVAL_MS, vibe_timer_callback, NULL);
  }
}

/* Stop the alarm: cancel the pending repeat and any vibration in progress. */
static void stop_done_vibe(void) {
  if (s_vibe_timer != NULL) {
    app_timer_cancel(s_vibe_timer);  // Frees the timer; the handle is now invalid...
    s_vibe_timer = NULL;             // ...so forget it.
  }
  vibes_cancel();
}

/*
 * Time is up: switch to MODE_DONE, forget the saved timer (so reopening the
 * app doesn't ring again) and start the repeating alarm.
 */
static void handle_timer_done(void) {
  if (s_mode == MODE_DONE) {
    return;  // Already ringing (e.g. the tick and the wakeup both noticed).
  }
  s_mode = MODE_DONE;
  tick_timer_service_unsubscribe();  // No more countdown updates needed.
  stop_timer();

  s_vibe_count = 1;
  play_done_vibe();
  s_vibe_timer = app_timer_register(DONE_VIBE_INTERVAL_MS, vibe_timer_callback, NULL);

  update_display();
}

/* Close the timer window and go back to the grid. */
static void close_timer_window(void) {
  window_stack_remove(s_window, true);  // Triggers window_unload() below.
}

// =============================================================================
// Callbacks: clock ticks and buttons
// =============================================================================

/*
 * TickTimerService callback: called by Pebble at the start of every second.
 *   tick_time:     current local time, owned by Pebble (unused here).
 *   units_changed: which units changed (unused; we only asked for seconds).
 */
static void handle_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  if (!s_state.is_paused && get_remaining_seconds() <= 0) {
    handle_timer_done();
  } else {
    update_display();
  }
}

/*
 * SELECT (short press).
 * Counting: pause/resume. Prompt: confirm the cancel. Done: dismiss.
 */
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_mode) {
    case MODE_COUNTING:
      if (s_state.is_paused) {
        resume_timer();
      } else {
        pause_timer();
      }
      update_display();
      break;
    case MODE_CONFIRM_CANCEL:
      stop_timer();
      close_timer_window();
      break;
    case MODE_DONE:
      close_timer_window();
      break;
  }
}

/* SELECT held for LONG_PRESS_MS: add a minute (only while counting). */
static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode != MODE_COUNTING) {
    return;
  }
  add_time();
  vibes_short_pulse();  // Feedback, since the screen change is small.
  update_display();
}

/*
 * BACK.
 * Counting: show the cancel prompt. Prompt: keep the timer and leave the app
 * (the wakeup will bring us back). Done: dismiss.
 *
 * Subscribing to BACK replaces its default "close window" behavior, which is
 * why we close windows ourselves here.
 */
static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_mode) {
    case MODE_COUNTING:
      s_mode = MODE_CONFIRM_CANCEL;
      update_display();
      break;
    case MODE_CONFIRM_CANCEL:
      // The timer is already saved and its wakeup scheduled, so it's safe to
      // exit. Popping every window ends the app.
      window_stack_pop_all(true);
      break;
    case MODE_DONE:
      close_timer_window();
      break;
  }
}

/* UP/DOWN: only used to dismiss the alarm ("press any button"). */
static void up_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode == MODE_DONE) {
    close_timer_window();
  }
}

/* Click config provider: wire buttons to handlers (see grid_window.c). */
static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  // Long click: (button, hold time, handler on press-down, handler on release).
  // We act as soon as the hold time is reached, so no release handler.
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_PRESS_MS, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, up_down_click_handler);
}

// =============================================================================
// Window lifecycle
// =============================================================================

/*
 * Create a white, centered TextLayer and add it to `parent`.
 *   parent:   layer to attach it to (the window's root layer).
 *   frame:    position and size.
 *   font_key: one of the FONT_KEY_* system font names.
 * Returns the new layer. ALLOC: the caller must text_layer_destroy() it.
 * Exists because we create four text layers with the same settings.
 */
static TextLayer *create_text_layer(Layer *parent, GRect frame, const char *font_key) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);  // Transparent.
  text_layer_set_text_color(layer, COLOR_TEXT);
  text_layer_set_font(layer, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(layer, GTextAlignmentCenter);
  layer_add_child(parent, text_layer_get_layer(layer));
  return layer;
}

/*
 * Window load: create all layers (ALLOC; all freed in window_unload()) and
 * start the once-per-second tick.
 */
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int ring_x = (bounds.size.w - RING_DIAMETER) / 2;
  int ring_center_y = RING_TOP + RING_DIAMETER / 2;

  s_ring_layer = layer_create(GRect(ring_x, RING_TOP, RING_DIAMETER, RING_DIAMETER));
  layer_set_update_proc(s_ring_layer, draw_progress_ring);
  layer_add_child(root, s_ring_layer);

  // Big MM:SS in the middle of the ring. LECO is a digits-only font.
  s_time_layer = create_text_layer(root,
      GRect(0, ring_center_y - TIME_TEXT_HEIGHT / 2 - 4, bounds.size.w, TIME_TEXT_HEIGHT),
      FONT_KEY_LECO_32_BOLD_NUMBERS);

  s_status_layer = create_text_layer(root,
      GRect(0, STATUS_TOP, bounds.size.w, STATUS_HEIGHT), FONT_KEY_GOTHIC_18_BOLD);

  int done_top = (bounds.size.h - DONE_TEXT_HEIGHT - DONE_HINT_HEIGHT) / 2;
  s_done_layer = create_text_layer(root,
      GRect(0, done_top, bounds.size.w, DONE_TEXT_HEIGHT), FONT_KEY_BITHAM_42_BOLD);
  text_layer_set_text(s_done_layer, TEXT_DONE);  // String literal: lives forever, safe.

  s_done_hint_layer = create_text_layer(root,
      GRect(0, done_top + DONE_TEXT_HEIGHT, bounds.size.w, DONE_HINT_HEIGHT), FONT_KEY_GOTHIC_18);
  text_layer_set_text(s_done_hint_layer, TEXT_DONE_HINT);

  if (s_mode != MODE_DONE) {
    tick_timer_service_subscribe(SECOND_UNIT, handle_second_tick);
  }
  update_display();
}

/*
 * Window unload: runs when the window leaves the stack. Stop every callback
 * that could touch our layers, then FREE each layer created in window_load()
 * and the window created in show_window(). After this, a stray callback
 * using a destroyed layer would crash the app, hence the unsubscribes first.
 */
static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  stop_done_vibe();

  text_layer_destroy(s_done_hint_layer);  // FREE (window_load)
  text_layer_destroy(s_done_layer);       // FREE (window_load)
  text_layer_destroy(s_status_layer);     // FREE (window_load)
  text_layer_destroy(s_time_layer);       // FREE (window_load)
  layer_destroy(s_ring_layer);            // FREE (window_load)
  s_done_hint_layer = NULL;
  s_done_layer = NULL;
  s_status_layer = NULL;
  s_time_layer = NULL;
  s_ring_layer = NULL;

  window_destroy(s_window);               // FREE (show_window)
  s_window = NULL;
}

/*
 * Create the window if needed and push it, or just refresh it if it's
 * already on screen.
 */
static void show_window(void) {
  if (s_window == NULL) {
    s_window = window_create();  // ALLOC: freed in window_unload()
    window_set_click_config_provider(s_window, click_config_provider);
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = window_load,
      .unload = window_unload,
    });
  }
  if (window_stack_contains_window(s_window)) {
    update_display();
  } else {
    window_stack_push(s_window, true);  // Calls window_load() before showing.
  }
}

// =============================================================================
// Public functions (declared in timer_window.h)
// =============================================================================

void timer_window_start(int minutes) {
  s_state = (TimerState) {
    .end_time = time(NULL) + minutes * SECONDS_PER_MINUTE,
    .total_seconds = minutes * SECONDS_PER_MINUTE,
    .paused_remaining = 0,
    .is_paused = false,
    .wakeup_id = NO_WAKEUP,
  };
  s_mode = MODE_COUNTING;
  schedule_wakeup();
  storage_save_timer(&s_state);
  show_window();
}

bool timer_window_resume_saved(void) {
  // Pass the address of s_state so storage_load_timer() can fill it in.
  if (!storage_load_timer(&s_state)) {
    return false;
  }
  s_mode = MODE_COUNTING;
  show_window();
  if (!s_state.is_paused && get_remaining_seconds() <= 0) {
    // Ran out while we were closed (e.g. the wakeup couldn't be scheduled).
    handle_timer_done();
  }
  return true;
}

void timer_window_show_done(void) {
  if (s_mode == MODE_DONE && s_window != NULL) {
    return;  // Already showing the alarm.
  }
  // Load the saved state so stop_timer() cancels the right wakeup. If
  // nothing is saved, s_state keeps its zeroed/previous values; that's fine.
  storage_load_timer(&s_state);
  s_mode = MODE_COUNTING;  // So handle_timer_done() doesn't return early.
  show_window();
  handle_timer_done();
}
