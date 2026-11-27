/*
 * grid_window.c - The 2x2 grid of timer buttons.
 *
 * HOW DRAWING WORKS ON PEBBLE
 * You don't draw whenever you like. Instead you give a Layer an "update
 * procedure" (a callback function). Pebble calls it whenever the layer needs
 * repainting, e.g. after you call layer_mark_dirty(). All our drawing
 * happens inside draw_grid(), which reads the current state (which square
 * is highlighted) and paints everything from scratch.
 *
 * INPUT
 * Buttons are always available. Touch is only compiled in when the SDK
 * defines PBL_TOUCH (the Pebble Time 2 / emery has a touchscreen), so the
 * same code still builds for watches without one.
 */
#include "grid_window.h"
#include "timer_window.h"

// ---- Layout constants (pixels) ---------------------------------------------
#define GRID_COLUMNS         2
#define GRID_ROWS            2
#define GRID_CELL_COUNT      (GRID_COLUMNS * GRID_ROWS)
#define GRID_MARGIN          6    // Space between the screen edge and the grid.
#define GRID_GAP             6    // Space between two squares.
#define CELL_CORNER_RADIUS   10
#define HIGHLIGHT_BORDER     3    // Thickness of the white outline on the highlight.
#define NUMBER_TEXT_HEIGHT   48   // Height reserved for the big number.
#define UNIT_TEXT_HEIGHT     22   // Height reserved for "min".

// ---- Colors -----------------------------------------------------------------
#define COLOR_GRID_BACKGROUND  GColorBlack
#define COLOR_CELL             GColorDarkGray
#define COLOR_CELL_HIGHLIGHT   GColorOrange
#define COLOR_CELL_TEXT        GColorWhite
#define COLOR_HIGHLIGHT_BORDER GColorWhite

// ---- Timer choices ----------------------------------------------------------
// One entry per square, in reading order (top-left, top-right, bottom-left,
// bottom-right). `static const` keeps the array private to this file and
// read-only.
static const int TIMER_MINUTES[GRID_CELL_COUNT] = { 5, 10, 15, 30 };

// ---- State ------------------------------------------------------------------
// `static` at file level means "only visible inside this file". These are
// pointers to objects that the Pebble SDK allocates on the heap for us;
// we must give them back with the matching *_destroy() function.
static Window *s_window;
static Layer *s_grid_layer;
static int s_selected = 0;  // Index (0..3) of the highlighted square.

#if defined(PBL_TOUCH)
static int s_touch_down_cell = -1;  // Square under the finger at touchdown, or -1.
#endif

/*
 * Compute the rectangle of one grid square.
 *   bounds: the area the whole grid lives in (the layer's bounds).
 *   index:  which square, 0..3 in reading order.
 * Returns the square's rectangle in the layer's coordinates.
 *
 * Exists so that drawing and touch hit-testing use the *same* math: if you
 * change the layout here, taps still land on the right square.
 */
static GRect cell_rect(GRect bounds, int index) {
  int column = index % GRID_COLUMNS;  // 0 = left, 1 = right
  int row = index / GRID_COLUMNS;     // 0 = top,  1 = bottom (integer division)

  int cell_width = (bounds.size.w - 2 * GRID_MARGIN - (GRID_COLUMNS - 1) * GRID_GAP) / GRID_COLUMNS;
  int cell_height = (bounds.size.h - 2 * GRID_MARGIN - (GRID_ROWS - 1) * GRID_GAP) / GRID_ROWS;

  return GRect(bounds.origin.x + GRID_MARGIN + column * (cell_width + GRID_GAP),
               bounds.origin.y + GRID_MARGIN + row * (cell_height + GRID_GAP),
               cell_width, cell_height);
}

/*
 * Find which square contains a point.
 *   bounds: the grid's area (same as for cell_rect()).
 *   point:  a position in the same coordinates.
 * Returns the square's index (0..3), or -1 if the point is in a gap/margin.
 */
static int cell_at_point(GRect bounds, GPoint point) {
  for (int i = 0; i < GRID_CELL_COUNT; i++) {
    GRect rect = cell_rect(bounds, i);
    // grect_contains_point() takes pointers, so we pass addresses with `&`.
    if (grect_contains_point(&rect, &point)) {
      return i;
    }
  }
  return -1;
}

/*
 * Draw one square: a rounded rectangle with "<minutes>" and "min" in it.
 *   ctx:         the graphics context Pebble gave to the update procedure;
 *                every graphics_* call needs it.
 *   rect:        where to draw the square.
 *   minutes:     the number to show.
 *   highlighted: true draws it in the highlight color with an outline.
 */
static void draw_cell(GContext *ctx, GRect rect, int minutes, bool highlighted) {
  // Background
  graphics_context_set_fill_color(ctx, highlighted ? COLOR_CELL_HIGHLIGHT : COLOR_CELL);
  graphics_fill_rect(ctx, rect, CELL_CORNER_RADIUS, GCornersAll);

  if (highlighted) {
    graphics_context_set_stroke_color(ctx, COLOR_HIGHLIGHT_BORDER);
    graphics_context_set_stroke_width(ctx, HIGHLIGHT_BORDER);
    graphics_draw_round_rect(ctx, rect, CELL_CORNER_RADIUS);
  }

  // Text. The number and "min" are stacked and centered vertically as a block.
  // A char array on the stack is enough room for "30" plus the '\0' terminator.
  char number_text[4];
  snprintf(number_text, sizeof(number_text), "%d", minutes);

  int block_height = NUMBER_TEXT_HEIGHT + UNIT_TEXT_HEIGHT;
  int top = rect.origin.y + (rect.size.h - block_height) / 2;
  GRect number_rect = GRect(rect.origin.x, top - 8, rect.size.w, NUMBER_TEXT_HEIGHT);
  GRect unit_rect = GRect(rect.origin.x, top + NUMBER_TEXT_HEIGHT - 4, rect.size.w, UNIT_TEXT_HEIGHT);
  // (The -8 / -4 compensate for the empty space fonts keep above the glyphs.)

  graphics_context_set_text_color(ctx, COLOR_CELL_TEXT);
  // fonts_get_system_font() returns a font built into the watch. System
  // fonts are never freed by us (unlike fonts loaded from resources).
  graphics_draw_text(ctx, number_text, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                     number_rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "min", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     unit_rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

/*
 * The grid layer's update procedure (a Pebble callback).
 *   layer: the layer being redrawn (s_grid_layer).
 *   ctx:   graphics context to draw with.
 * Pebble calls this, never us directly. Paints all four squares.
 */
static void draw_grid(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  for (int i = 0; i < GRID_CELL_COUNT; i++) {
    draw_cell(ctx, cell_rect(bounds, i), TIMER_MINUTES[i], i == s_selected);
  }
}

/*
 * Change which square is highlighted and ask Pebble to redraw.
 *   index: new square, 0..3.
 */
static void set_selected(int index) {
  s_selected = index;
  layer_mark_dirty(s_grid_layer);  // "Please call draw_grid() soon."
}

/*
 * Start the timer for the highlighted square. The timer window takes over
 * from here (it saves state, schedules the wakeup and shows itself).
 */
static void start_selected_timer(void) {
  timer_window_start(TIMER_MINUTES[s_selected]);
}

// ---- Button handlers (Pebble callbacks) -------------------------------------
// Signature required by the SDK: (ClickRecognizerRef recognizer, void *context).
// We don't need either argument here.

/* UP: move the highlight one square back, wrapping from 0 to 3. */
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  set_selected((s_selected + GRID_CELL_COUNT - 1) % GRID_CELL_COUNT);
}

/* DOWN: move the highlight one square forward, wrapping from 3 to 0. */
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  set_selected((s_selected + 1) % GRID_CELL_COUNT);
}

/* SELECT: start the highlighted timer. */
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  start_selected_timer();
}

/*
 * Click config provider (a Pebble callback): Pebble calls this once when the
 * window becomes active, and we tell it which handler goes with which button.
 * BACK is not subscribed, so it keeps its default meaning (exit the app).
 */
static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

#if defined(PBL_TOUCH)
/*
 * Touch handler (a Pebble TouchService callback).
 *   event:   what happened (touchdown / position update / liftoff) and where,
 *            in screen pixels. Pebble owns this memory; only read it here.
 *   context: the pointer we passed to touch_service_subscribe() (unused).
 *
 * A "tap" = finger goes down on a square and lifts off on the same square.
 * Touchdown highlights the square right away as feedback; liftoff starts it.
 * Requiring the same square on both ends ignores swipes across the grid.
 */
static void handle_touch(const TouchEvent *event, void *context) {
  if (event->non_navigational) {
    return;  // The system says this contact shouldn't trigger actions.
  }

  // Our grid layer fills the whole window at (0,0), so screen coordinates
  // equal layer coordinates and we can hit-test directly.
  GRect bounds = layer_get_bounds(s_grid_layer);
  int cell = cell_at_point(bounds, GPoint(event->x, event->y));

  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_down_cell = cell;
      if (cell >= 0) {
        set_selected(cell);
      }
      break;
    case TouchEvent_Liftoff:
      if (cell >= 0 && cell == s_touch_down_cell) {
        start_selected_timer();
      }
      s_touch_down_cell = -1;
      break;
    default:
      break;  // TouchEvent_PositionUpdate: finger moving, nothing to do.
  }
}
#endif

// ---- Window lifecycle (Pebble callbacks) ------------------------------------

/*
 * Called when the window is pushed and about to be shown for the first time.
 * Create the layers here. layer_create() allocates memory on the heap, which
 * we MUST free in window_unload() or the app leaks memory.
 */
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);  // Owned by the window; don't destroy.
  GRect bounds = layer_get_bounds(root);

  s_grid_layer = layer_create(bounds);                  // ALLOC: freed in window_unload()
  layer_set_update_proc(s_grid_layer, draw_grid);       // Hand Pebble a function pointer.
  layer_add_child(root, s_grid_layer);                  // Attach so it gets drawn.
}

/*
 * Called every time the window becomes the visible (top) window, including
 * when we come back to it after the timer screen is closed.
 * Touch is subscribed only while the grid is visible, so taps on the timer
 * screen can't accidentally start a new timer.
 */
static void window_appear(Window *window) {
#if defined(PBL_TOUCH)
  s_touch_down_cell = -1;
  touch_service_subscribe(handle_touch, NULL);
#endif
}

/* Called when another window covers this one or it is removed. */
static void window_disappear(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
}

/*
 * Called when the window is popped off the stack (e.g. the app exits).
 * FREE everything created in window_load(), then the window itself, because
 * we created it in grid_window_push(). Setting the pointers to NULL avoids
 * "dangling pointers" that point at freed memory.
 */
static void window_unload(Window *window) {
  layer_destroy(s_grid_layer);  // FREE: matches layer_create() in window_load()
  s_grid_layer = NULL;

  window_destroy(s_window);     // FREE: matches window_create() in grid_window_push()
  s_window = NULL;
}

/*
 * Create and show the grid window (see grid_window.h).
 */
void grid_window_push(void) {
  if (s_window == NULL) {
    s_window = window_create();  // ALLOC: freed in window_unload()
    window_set_background_color(s_window, COLOR_GRID_BACKGROUND);
    window_set_click_config_provider(s_window, click_config_provider);
    // WindowHandlers is a struct of function pointers; Pebble calls each one
    // at the matching moment in the window's life.
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = window_load,
      .appear = window_appear,
      .disappear = window_disappear,
      .unload = window_unload,
    });
  }
  window_stack_push(s_window, true);  // true = animate the transition.
}
