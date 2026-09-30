#pragma once
#include <lvgl.h>
// Globe: the Earth as the Sun sees it right now, with Doug's cities on it. A port of the
// background globe on Doug's homepage. The cities come from orb-ponderer (`globe.cities`),
// with a built-in list until the relay answers.
//
// Same shape as every other app: init() builds the screen and registers the
// ponderer::Module, the shell calls onEnter/onExit as it comes and goes, onPress/onTurn
// are the knob (a turn spins the globe, a press toggles city names).
namespace globeview {
    void      init();
    lv_obj_t* screen();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      onTurn(int delta);
}
