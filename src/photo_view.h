#pragma once
#include <lvgl.h>
// Photos: A random photo from a pixel album (through orb-ponderer, fn=photo.*) or from /photos on the SD card.
//
// Same shape as every other app: init() builds the screen (and registers any
// ponderer::Module), the shell calls onEnter/onExit as it comes and goes, and
// onPress/onTurn are the knob (onTurn only arrives while the app has captured it,
// see app_shell::setCaptured).
namespace photoview {
    void      init();
    lv_obj_t* screen();
    void      onEnter();
    void      onExit();
    void      onPress();
    void      onTurn(int delta);
}
