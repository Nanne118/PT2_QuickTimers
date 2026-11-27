/*
 * storage.c - Persistent storage for the active timer.
 *
 * Pebble's persistent storage is a small key/value store that survives the
 * app closing and the watch rebooting. Each value is identified by a
 * uint32_t key that we choose. Each app has its own store, so our keys
 * cannot collide with other apps.
 *
 * We store the whole TimerState struct as one block of raw bytes. That is
 * simple, but if the struct's layout ever changes (a field added, removed or
 * reordered), old saved bytes would be misread. To guard against that we
 * also store a version number and ignore data saved by an older version.
 */
#include "storage.h"

// Keys in the persistent key/value store. Any distinct numbers work.
#define PERSIST_KEY_VERSION  1
#define PERSIST_KEY_TIMER    2

// Bump this whenever you change the TimerState struct (see comment above).
#define STORAGE_VERSION      1

/*
 * Save the timer state (see storage.h).
 *
 * persist_write_data() copies `sizeof(TimerState)` bytes starting at the
 * address `state` points to. The max size of one value is 256 bytes
 * (PERSIST_DATA_MAX_LENGTH); our struct is about 20 bytes.
 */
void storage_save_timer(const TimerState *state) {
  persist_write_int(PERSIST_KEY_VERSION, STORAGE_VERSION);
  persist_write_data(PERSIST_KEY_TIMER, state, sizeof(TimerState));
}

/*
 * Load the timer state (see storage.h).
 *
 * We first check that both keys exist and the version matches, then read
 * into a temporary struct. Only if the read returned the exact number of
 * bytes we expected do we copy it to *out. That way a half-read or
 * outdated record never reaches the rest of the app.
 */
bool storage_load_timer(TimerState *out) {
  if (!persist_exists(PERSIST_KEY_TIMER) || !persist_exists(PERSIST_KEY_VERSION)) {
    return false;
  }
  if (persist_read_int(PERSIST_KEY_VERSION) != STORAGE_VERSION) {
    return false;
  }

  TimerState loaded;  // Local variable on the stack, freed automatically on return.
  // persist_read_data() fills `loaded` and returns how many bytes it wrote
  // (or a negative error code).
  int bytes_read = persist_read_data(PERSIST_KEY_TIMER, &loaded, sizeof(TimerState));
  if (bytes_read != (int)sizeof(TimerState)) {
    return false;
  }

  *out = loaded;  // Struct assignment: copies every field into the caller's struct.
  return true;
}

/*
 * Remove the saved timer (see storage.h).
 */
void storage_clear_timer(void) {
  persist_delete(PERSIST_KEY_TIMER);
  persist_delete(PERSIST_KEY_VERSION);
}
