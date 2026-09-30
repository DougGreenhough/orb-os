#pragma once
#include <lvgl.h>
// Plasma: A plasma ball filling the round glass, ported from the plasma project's discharge engine. No network.
//
// Same shape as every other app: init() builds the screen (and registers any
// ponderer::Module), the shell calls onEnter/onExit as it comes and goes, and
// onPress/onTurn are the knob (onTurn only arrives while the app has captured it,
// see app_shell::setCaptured).
namespace plasmaview {
    void      init();
    lv_obj_t* screen();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      onTurn(int delta);
}
