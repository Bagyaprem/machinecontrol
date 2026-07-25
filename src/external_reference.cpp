#include "external_reference.h"

static bool s_hasData = false;
static float s_co2 = 0;
static float s_pm25 = 0;

void external_reference_set(float co2, float pm25) {
  s_co2 = co2;
  s_pm25 = pm25;
  s_hasData = true;
}

bool external_reference_has_data() { return s_hasData; }
float external_reference_get_co2() { return s_co2; }
float external_reference_get_pm25() { return s_pm25; }
