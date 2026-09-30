// Facts: placeholder until the real screen lands.
#include "facts_view.h"

namespace { lv_obj_t *s_scr = nullptr; }

namespace factsview {

void init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A8F98), 0);
    lv_label_set_text(l, "Facts");
    lv_obj_center(l);
}

lv_obj_t *screen() { return s_scr; }
void onEnter() {}
void onExit() {}
void onPress() {}
void onTurn(int) {}

} // namespace factsview
