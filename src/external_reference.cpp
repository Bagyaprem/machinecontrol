#include "external_reference.h"

static bool s_hasData = false;
static float s_pm25 = 0;

void external_reference_set(float pm25) {
  s_pm25 = pm25;
  s_hasData = true;
}

bool external_reference_has_data() { return s_hasData; }
float external_reference_get_pm25() { return s_pm25; }
