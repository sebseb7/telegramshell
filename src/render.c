#include "render.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <jpeglib.h>
#include "font8x16.h"

#define COLS 150
#define ROWS 100

typedef struct {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
} cell_t;

struct vterm_s {
    cell_t grid[ROWS][COLS];
    int cx;
    int cy;
    unsigned char current_fg;
    unsigned char current_bg;
    
    int state;
    int params[16];
    int nparams;
    
    int utf8_state;
    unsigned int utf8_cp;
};

static const unsigned char colors_r[16] = { 0, 205, 0, 205, 0, 205, 0, 229, 127, 255, 0, 255, 92, 255, 0, 255 };
static const unsigned char colors_g[16] = { 0, 0, 205, 205, 0, 0, 205, 229, 127, 0, 255, 255, 92, 0, 255, 255 };
static const unsigned char colors_b[16] = { 0, 0, 0, 0, 238, 205, 205, 229, 127, 0, 0, 0, 255, 255, 255, 255 };

vterm_t *vterm_create(void) {
    vterm_t *vt = (vterm_t *)calloc(1, sizeof(vterm_t));
    vterm_reset(vt);
    return vt;
}

void vterm_free(vterm_t *vt) {
    free(vt);
}

void vterm_reset(vterm_t *vt) {
    memset(vt->grid, 0, sizeof(vt->grid));
    vt->cx = 0;
    vt->cy = 0;
    vt->current_fg = 7;
    vt->current_bg = 0;
    vt->state = 0;
    vt->nparams = 0;
    vt->utf8_state = 0;
    vt->utf8_cp = 0;
}

void vterm_clear_screen(vterm_t *vt) {
    memset(vt->grid, 0, sizeof(vt->grid));
    vt->cx = 0;
    vt->cy = 0;
}

static void scroll_up(vterm_t *vt) {
    memmove(vt->grid[0], vt->grid[1], sizeof(cell_t) * (ROWS - 1) * COLS);
    memset(vt->grid[ROWS - 1], 0, sizeof(cell_t) * COLS);
}

static void put_char(vterm_t *vt, unsigned int cp) {
    if (cp == '\n') {
        vt->cy++;
        if (vt->cy >= ROWS) {
            vt->cy = ROWS - 1;
            scroll_up(vt);
        }
    } else if (cp == '\r') {
        vt->cx = 0;
    } else if (cp == '\b') {
        if (vt->cx > 0) vt->cx--;
    } else if (cp >= 32) {
        unsigned char ch = '?';
        if (cp < 128) ch = (unsigned char)cp;
        else if (cp == 0x2500) ch = 128; // ─
        else if (cp == 0x2502) ch = 129; // │
        else if (cp == 0x250C) ch = 130; // ┌
        else if (cp == 0x2510) ch = 131; // ┐
        else if (cp == 0x2514) ch = 132; // └
        else if (cp == 0x2518) ch = 133; // ┘
        else if (cp == 0x251C) ch = 134; // ├
        else if (cp == 0x2524) ch = 135; // ┤
        else if (cp == 0x252C) ch = 136; // ┬
        else if (cp == 0x2534) ch = 137; // ┴
        else if (cp == 0x253C) ch = 138; // ┼
        else if (cp == 0x256D) ch = 130; // ╭ -> ┌
        else if (cp == 0x256E) ch = 131; // ╮ -> ┐
        else if (cp == 0x256F) ch = 133; // ╯ -> ┘
        else if (cp == 0x2570) ch = 132; // ╰ -> └
        
        if (vt->cx >= COLS) {
            vt->cx = 0;
            vt->cy++;
            if (vt->cy >= ROWS) {
                vt->cy = ROWS - 1;
                scroll_up(vt);
            }
        }
        vt->grid[vt->cy][vt->cx].ch = ch;
        vt->grid[vt->cy][vt->cx].fg = vt->current_fg;
        vt->grid[vt->cy][vt->cx].bg = vt->current_bg;
        vt->cx++;
    }
}

static void apply_sgr(vterm_t *vt) {
    if (vt->nparams == 0) {
        vt->current_fg = 7;
        vt->current_bg = 0;
        return;
    }
    for (int i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];
        if (p == 0) { vt->current_fg = 7; vt->current_bg = 0; }
        else if (p >= 30 && p <= 37) vt->current_fg = p - 30;
        else if (p == 39) vt->current_fg = 7;
        else if (p >= 40 && p <= 47) vt->current_bg = p - 40;
        else if (p == 49) vt->current_bg = 0;
        else if (p >= 90 && p <= 97) vt->current_fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) vt->current_bg = p - 100 + 8;
        else if (p == 1) vt->current_fg |= 8;
    }
}

void vterm_process(vterm_t *vt, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];
        
        if (vt->utf8_state > 0) {
            if ((c & 0xC0) == 0x80) {
                vt->utf8_cp = (vt->utf8_cp << 6) | (c & 0x3F);
                vt->utf8_state--;
                if (vt->utf8_state == 0) {
                    if (vt->state == 0) put_char(vt, vt->utf8_cp);
                }
            } else {
                vt->utf8_state = 0;
            }
            continue;
        }
        if (c >= 0xC0) {
            if ((c & 0xE0) == 0xC0) { vt->utf8_state = 1; vt->utf8_cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { vt->utf8_state = 2; vt->utf8_cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { vt->utf8_state = 3; vt->utf8_cp = c & 0x07; }
            continue;
        }

        if (vt->state == 0) {
            if (c == 0x1B) {
                vt->state = 1;
            } else {
                put_char(vt, c);
            }
        } else if (vt->state == 1) {
            if (c == '[') {
                vt->state = 2;
                vt->nparams = 0;
                vt->params[0] = 0;
            } else if (c == ']') {
                vt->state = 3; // OSC
            } else {
                vt->state = 0;
            }
        } else if (vt->state == 2) {
            if (c >= '0' && c <= '9') {
                if (vt->nparams == 0) vt->nparams = 1;
                vt->params[vt->nparams - 1] = vt->params[vt->nparams - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (vt->nparams < 16) {
                    if (vt->nparams == 0) vt->nparams = 1;
                    vt->params[vt->nparams] = 0;
                    vt->nparams++;
                }
            } else if (c == '?') {
                // Ignore private sequence indicator
            } else {
                if (c == 'm') {
                    apply_sgr(vt);
                } else if (c == 'K') {
                    int p = vt->nparams > 0 ? vt->params[0] : 0;
                    if (p == 0) {
                        for (int k = vt->cx; k < COLS; k++) { vt->grid[vt->cy][k].ch = 0; vt->grid[vt->cy][k].bg = vt->current_bg; }
                    } else if (p == 1) {
                        for (int k = 0; k <= vt->cx && k < COLS; k++) { vt->grid[vt->cy][k].ch = 0; vt->grid[vt->cy][k].bg = vt->current_bg; }
                    } else if (p == 2) {
                        for (int k = 0; k < COLS; k++) { vt->grid[vt->cy][k].ch = 0; vt->grid[vt->cy][k].bg = vt->current_bg; }
                    }
                } else if (c == 'J') {
                    int p = vt->nparams > 0 ? vt->params[0] : 0;
                    if (p == 2) {
                        memset(vt->grid, 0, sizeof(vt->grid));
                        vt->cx = 0; vt->cy = 0;
                    }
                } else if (c == 'H' || c == 'f') {
                    int r = vt->nparams > 0 ? vt->params[0] : 1;
                    int c_idx = vt->nparams > 1 ? vt->params[1] : 1;
                    if (r < 1) r = 1; 
                    if (r > ROWS) r = ROWS;
                    if (c_idx < 1) c_idx = 1; 
                    if (c_idx > COLS) c_idx = COLS;
                    vt->cy = r - 1;
                    vt->cx = c_idx - 1;
                }
                vt->state = 0;
            }
        } else if (vt->state == 3) {
            if (c == 0x07) vt->state = 0;
            else if (c == 0x1B) vt->state = 4;
        } else if (vt->state == 4) {
            if (c == '\\') vt->state = 0;
            else vt->state = 3;
        }
    }
}

void vterm_render_jpeg(vterm_t *vt, unsigned char **out_jpeg, unsigned long *out_len) {
    int max_y = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        int has_char = 0;
        for (int x = 0; x < COLS; x++) {
            if (vt->grid[y][x].ch != 0 || vt->grid[y][x].bg != 0) {
                has_char = 1; break;
            }
        }
        if (has_char) { max_y = y; break; }
    }
    
    int img_w = COLS * 8;
    int img_h = (max_y + 1) * 16;
    unsigned char *rgb = (unsigned char *)malloc(img_w * img_h * 3);
    memset(rgb, 0, img_w * img_h * 3);
    
    for (int y = 0; y <= max_y; y++) {
        for (int x = 0; x < COLS; x++) {
            cell_t c = vt->grid[y][x];
            unsigned char ch = c.ch ? c.ch : ' ';
            unsigned char fg = c.fg & 15;
            unsigned char bg = c.bg & 15;
            const unsigned char *glyph;
            unsigned char custom_glyph[16] = {0};
            
            if (ch < 128) {
                glyph = font8x16 + (ch * 16);
            } else {
                int up = 0, down = 0, left = 0, right = 0;
                if (ch==128) { left=1; right=1; }
                else if (ch==129) { up=1; down=1; }
                else if (ch==130) { right=1; down=1; }
                else if (ch==131) { left=1; down=1; }
                else if (ch==132) { up=1; right=1; }
                else if (ch==133) { up=1; left=1; }
                else if (ch==134) { up=1; down=1; right=1; }
                else if (ch==135) { up=1; down=1; left=1; }
                else if (ch==136) { left=1; right=1; down=1; }
                else if (ch==137) { left=1; right=1; up=1; }
                else if (ch==138) { up=1; down=1; left=1; right=1; }
                
                for (int r = 0; r < 16; r++) {
                    unsigned char row = 0;
                    if (r < 8 && up) row |= 0x10;
                    if (r > 8 && down) row |= 0x10;
                    if (r == 8) {
                        if (up || down) row |= 0x10;
                        if (left) row |= 0xF0;
                        if (right) row |= 0x1F;
                    }
                    custom_glyph[r] = row;
                }
                glyph = custom_glyph;
            }

            for (int r = 0; r < 16; r++) {
                unsigned char row = glyph[r];
                for (int p = 0; p < 8; p++) {
                    int px = x * 8 + p;
                    int py = y * 16 + r;
                    int idx = (py * img_w + px) * 3;
                    if (row & (1 << (7 - p))) {
                        rgb[idx] = colors_r[fg];
                        rgb[idx+1] = colors_g[fg];
                        rgb[idx+2] = colors_b[fg];
                    } else {
                        rgb[idx] = colors_r[bg];
                        rgb[idx+1] = colors_g[bg];
                        rgb[idx+2] = colors_b[bg];
                    }
                }
            }
        }
    }
    
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    
    jpeg_mem_dest(&cinfo, out_jpeg, out_len);
    
    cinfo.image_width = img_w;
    cinfo.image_height = img_h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    
    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb[cinfo.next_scanline * img_w * 3];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(rgb);
}
