#pragma once
#include <lvgl.h>
// Music: Spotify now-playing and playback control, through orb-ponderer (fn=spotify.*).
//
// Same shape as every other app: init() builds the screen (and registers any
// ponderer::Module), the shell calls onEnter/onExit as it comes and goes, and
// onPress/onTurn are the knob (onTurn only arrives while the app has captured it,
// see app_shell::setCaptured).
namespace musicview {
    void      init();
    lv_obj_t* screen();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      onTurn(int delta);
}
