#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>

typedef struct vterm_s vterm_t;

vterm_t *vterm_create(void);
void vterm_free(vterm_t *vt);
void vterm_reset(vterm_t *vt);
void vterm_clear_screen(vterm_t *vt);
void vterm_process(vterm_t *vt, const char *data, size_t len);
void vterm_render_jpeg(vterm_t *vt, unsigned char **out_jpeg, unsigned long *out_len);

#endif
