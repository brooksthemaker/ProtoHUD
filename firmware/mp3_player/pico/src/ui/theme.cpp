#include "theme.h"

void Theme::apply(lv_disp_t* disp) {
    lv_theme_t* th = lv_theme_default_init(
        disp,
        ACCENT(),
        ACCENT2(),
        /*dark=*/true,
        &lv_font_montserrat_14
    );
    lv_disp_set_theme(disp, th);

    // Override background colour.
    lv_obj_set_style_bg_color(lv_scr_act(), BG(), 0);
}
