#pragma once
#include <lvgl.h>
// Forecast: current conditions and the next four days, read from the same WeatherSnapshot
// the (launch-one-disabled) radar's forecast panel uses. No network of its own: the device
// fetches from its network task (main.cpp, WEATHER_REFRESH_MS) and the simulator fetches
// once at start-up. Drawn from plain LVGL objects, so it takes no PSRAM canvas.
namespace forecastview {
    void      init();       // build the screen (core 1 / LVGL)
    lv_obj_t* screen();
    void      onEnter();    // redraw from the latest snapshot
    void      onExit();
    void      refresh();    // a new snapshot landed (core 1 only: it writes LVGL objects)
}
