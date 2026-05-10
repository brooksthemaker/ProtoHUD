#include "album_art.h"
#include "theme.h"
#include <JPEGDEC.h>

lv_color_t AlbumArt::s_px_buf_[AlbumArt::W * AlbumArt::H];

static JPEGDEC s_jpeg;

// JPEGDEC MCU draw callback — writes decoded RGB565 blocks into the canvas buffer.
// Pixel type is set to RGB565_BIG_ENDIAN which matches LV_COLOR_16_SWAP=1.
static int jpeg_draw_cb(JPEGDRAW* pDraw) {
    lv_color_t* buf = AlbumArt::px_buf();
    uint16_t*   src = pDraw->pPixels;
    for (int row = 0; row < pDraw->iHeight; row++) {
        for (int col = 0; col < pDraw->iWidth; col++) {
            int px_x = pDraw->x + col;
            int px_y = pDraw->y + row;
            if (px_x < AlbumArt::W && px_y < AlbumArt::H && px_x >= 0 && px_y >= 0)
                buf[px_y * AlbumArt::W + px_x].full = src[row * pDraw->iWidth + col];
        }
    }
    return 1;
}

void AlbumArt::create(lv_obj_t* parent, int x, int y) {
    canvas_ = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_, s_px_buf_, W, H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas_, x, y);
    show_placeholder();
}

bool AlbumArt::load_jpeg(const uint8_t* jpeg_data, size_t len) {
    if (!canvas_ || !jpeg_data || len < 4) return false;

    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    s_jpeg.setPixelType(RGB565_BIG_ENDIAN);

    if (!s_jpeg.openRAM(const_cast<uint8_t*>(jpeg_data), static_cast<int>(len), jpeg_draw_cb)) {
        show_placeholder();
        return false;
    }

    // Pick scale so the decoded output fits within W×H.
    const int iw = s_jpeg.getWidth();
    const int ih = s_jpeg.getHeight();
    int scale;
    if      (iw > W * 4 || ih > H * 4)  scale = JPEG_SCALE_EIGHTH;
    else if (iw > W * 2 || ih > H * 2)  scale = JPEG_SCALE_QUARTER;
    else if (iw > W     || ih > H)       scale = JPEG_SCALE_HALF;
    else                                  scale = JPEG_SCALE_FULL;

    // Centre the scaled image in the 80×80 canvas.
    const int divisor = 1 << scale;
    const int off_x   = (W - iw / divisor) / 2;
    const int off_y   = (H - ih / divisor) / 2;

    s_jpeg.decode(off_x > 0 ? off_x : 0, off_y > 0 ? off_y : 0, scale);
    s_jpeg.close();
    lv_obj_invalidate(canvas_);
    return true;
}

void AlbumArt::show_placeholder() {
    if (!canvas_) return;
    lv_canvas_fill_bg(canvas_, Theme::SURFACE(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = Theme::ACCENT();
    rect_dsc.bg_opa   = LV_OPA_20;
    rect_dsc.radius   = 6;
    lv_canvas_draw_rect(canvas_, 2, 2, W - 4, H - 4, &rect_dsc);

    lv_draw_label_dsc_t lbl_dsc;
    lv_draw_label_dsc_init(&lbl_dsc);
    lbl_dsc.color = Theme::TEXT_DIM();
    lbl_dsc.font  = &lv_font_montserrat_24;
    lv_canvas_draw_text(canvas_, (W / 2) - 12, (H / 2) - 14, 30, &lbl_dsc, LV_SYMBOL_AUDIO);

    lv_obj_invalidate(canvas_);
}
