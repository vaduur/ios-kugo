/* sweetgl: Mesa GLES on the Adreno 530 (ios-sailfish) and 510 (kugo) (sweetgl.c). */
#ifndef SWEETGL_H
#define SWEETGL_H

#include <stdbool.h>

struct sweetgl_ctx;

#define SWEETGL_API __attribute__((visibility("default")))

/* Screen over SweetGPU; false while the GPU service is not up. */
SWEETGL_API bool sweetgl_init(void);

/* GLES api (2 or 3) context sharing objects with share (may be NULL). */
SWEETGL_API struct sweetgl_ctx *sweetgl_create(int api, struct sweetgl_ctx *share);

/* Current on this thread without a drawable (NULL: release). */
SWEETGL_API bool sweetgl_make_current(struct sweetgl_ctx *c);

SWEETGL_API void sweetgl_destroy(struct sweetgl_ctx *c);

/* Offscreen triangle, read back and checked; result in the kernel log. */
SWEETGL_API bool sweetgl_selftest(void);

/* internal (eagl.c) */
struct pipe_screen *sweetgl_screen(void);
struct pipe_frontend_screen;
void sweetgl_image_hooks(struct pipe_frontend_screen *fscreen);
void sweetgl_eagl_install(void);

#endif
