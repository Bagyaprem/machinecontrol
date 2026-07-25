#include "persist.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences s_prefs;
static bool s_ready = false;

void persist_init() {
  // Namespace "devstate" in the default NVS partition (the boot log's
  // 16KB "nvs" entry) - plenty for five small keys.
  s_ready = s_prefs.begin("devstate", false);
  if (!s_ready) Serial.println("[persist] NVS begin failed - last-command restore disabled this boot");
}

bool persist_get_bool(const char *key, bool def) {
  if (!s_ready) return def;
  return s_prefs.getBool(key, def);
}

void persist_set_bool(const char *key, bool value) {
  if (!s_ready) return;
  s_prefs.putBool(key, value);
}

int persist_get_int(const char *key, int def) {
  if (!s_ready) return def;
  return s_prefs.getInt(key, def);
}

void persist_set_int(const char *key, int value) {
  if (!s_ready) return;
  s_prefs.putInt(key, value);
}
