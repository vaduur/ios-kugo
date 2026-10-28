/*
 * EAGL on sweetgl: QuartzCore's GLES renderer (CA::OGL::GLESContext) on
 * Mesa instead of Apple's Metal-backed GLEngine.
 *
 * CA uses EAGLContext only through initWithAPI:properties:, API,
 * setParameter:to:, getMacroContextPrivate, texImageIOSurface:...,
 * attachImage:toCoreSurface:invertedRender:, +setCurrentContext: and
 * +currentContext; every GL call goes through the GLI dispatch table it gets
 * from getMacroContextPrivate ({rctx, disp[1036]}, layout from GLEngine,
 * table generated into out/dispatch.c). Those methods are replaced here for
 * contexts we create; the original EAGL code still runs for everything else.
 *
 * IOSurfaces become texture storage the EGL way: imported as a GEM bo
 * (SweetGPU selector 12), wrapped in a pipe_resource, handed to
 * glEGLImageTargetTexture2DOES through the frontend's get_egl_image.
 */
#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/vm_page_size.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>

#include "eagl.h"

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "util/u_inlines.h"
#include "drm-uapi/drm_fourcc.h"

#include "sweet_drm.h"
#include "sweetgl.h"

/* not in the GL bridge (ES-only): Mesa's implementation */
void _mesa_EGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image);

struct sgl_context {
	void *rctx;                     /* = this; CA reads {rctx, disp[]} from macro */
	void *disp[SGL_SLOTS];
	id eagl;
	unsigned api;
	struct sweetgl_ctx *ctx;
};

struct sgl_image {
	struct pipe_resource *res;
	enum pipe_format format;
	unsigned internalformat;
};

__thread void *sgl_current;
int sgl_check_errors;
static int trace_left;          /* SWEETGL_TRACE=1: this many trace lines */
static bool dump_shaders;       /* SWEETGL_TRACE=1: every shader source, no limit */

#define TRACE(...) do { if (trace_left > 0) { trace_left--; sweet_drm_log("sweetgl: trace " __VA_ARGS__); } } while (0)

/*
 * SWEETGL_TRACE: draws are traced in windows, every draw of one (state,
 * uniforms, the scissor box before and after): WIN_DRAWS draws from a draw
 * into a WIN_TRIGGER-sized viewport (CC slider modules, App Library pods),
 * then none for WIN_SECS.
 */
#define WIN_SECS 30
#define WIN_DRAWS 500
#define WIN_TRIGGER(w, h) (((w) == 138 && (h) == 306) || ((w) == 296 && (h) == 296) || ((w) == 297 && (h) == 296))
static int win_left;
static time_t win_next;
#define WTRACE(...) do { if (win_left > 0) TRACE(__VA_ARGS__); } while (0)

/* ponytail: linear table of contexts; backboardd makes one or two */
#define MAX_CTX 32
static struct sgl_context *ctxs[MAX_CTX];
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

static IMP orig_init, orig_dealloc, orig_api, orig_set_param, orig_macro, orig_tex_iosurface, orig_attach,
    orig_set_current, orig_current;

static struct sgl_context *
lookup(id eagl)
{
	struct sgl_context *c = NULL;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && c == NULL && eagl != NULL; i++) {
		if (ctxs[i] && ctxs[i]->eagl == eagl) {
			c = ctxs[i];
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	return c;
}

void
sgl_bind(void *rctx)
{
	struct sgl_context *c = rctx;
	sweetgl_make_current(c ? c->ctx : NULL);
	sgl_current = c;
}

void
sgl_unsupported(int slot, const char *name)
{
	static unsigned char seen[SGL_SLOTS];
	if (slot >= 0 && slot < SGL_SLOTS && !seen[slot]) {
		seen[slot] = 1;
		sweet_drm_log("sweetgl: unsupported GLI slot %d (%s)", slot, name);
	}
}

void
sgl_post(int slot)
{
	static unsigned char logged[SGL_SLOTS][8];     /* per slot and error, the first 3 */
	static bool used[SGL_SLOTS];
	if (!used[slot]) {
		used[slot] = true;
		sweet_drm_log("sweetgl: first call of slot %d %s", slot, sgl_slot_names[slot]);
	}
	GLenum e = glGetError();
	unsigned k = e - GL_INVALID_ENUM;
	if (e != GL_NO_ERROR && (k >= 8 || logged[slot][k] < 3)) {
		if (k < 8) {
			logged[slot][k]++;
		}
		sweet_drm_log("sweetgl: GL error %#x after slot %d %s", e, slot, sgl_slot_names[slot]);
	}
}

/* ---- hand-written slots ---- */

#define H(o) ((GLuint)(uintptr_t)(o))

void
sgl_DeleteObjectARB(void *rctx, GLhandleARB obj)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glDeleteProgram(H(obj));
	} else {
		glDeleteShader(H(obj));
	}
}

void
sgl_GetObjectParameterivARB(void *rctx, GLhandleARB obj, GLenum pname, GLint *params)
{
	SGL_CUR(rctx);
	if (pname == GL_OBJECT_TYPE_ARB) {
		*params = glIsProgram(H(obj)) ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB;
	} else if (glIsProgram(H(obj))) {
		glGetProgramiv(H(obj), pname, params);
	} else {
		glGetShaderiv(H(obj), pname == GL_OBJECT_SUBTYPE_ARB ? GL_SHADER_TYPE : pname, params);
	}
}

void
sgl_GetInfoLogARB(void *rctx, GLhandleARB obj, GLsizei max, GLsizei *length, GLcharARB *log)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glGetProgramInfoLog(H(obj), max, length, log);
	} else {
		glGetShaderInfoLog(H(obj), max, length, log);
	}
	if (log && max > 0 && log[0]) {
		sweet_drm_log("sweetgl: info log %u: %.300s", H(obj), log);
	}
}

void
sgl_GetAttachedObjectsARB(void *rctx, GLhandleARB prog, GLsizei max, GLsizei *count, GLhandleARB *obj)
{
	SGL_CUR(rctx);
	GLuint s[16];
	GLsizei n = 0;
	glGetAttachedShaders(H(prog), max < 16 ? max : 16, &n, s);
	for (GLsizei i = 0; i < n; i++) {
		obj[i] = (GLhandleARB)(uintptr_t)s[i];
	}
	if (count) {
		*count = n;
	}
}

GLhandleARB
sgl_GetHandleARB(void *rctx, GLenum pname)
{
	(void)rctx;
	(void)pname;
	return 0;
}

void
sgl_DiscardFramebufferEXT(void *rctx, GLenum target, GLsizei n, const GLenum *attachments)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	WTRACE("DiscardFramebuffer fb %d n %d first %#x", fb, n, n > 0 ? attachments[0] : 0);
	glInvalidateFramebuffer(target, n, attachments);
}

void
sgl_GetShaderPrecisionFormatOES(void *rctx, GLenum shader, GLenum precision, GLint *range, GLint *prec)
{
	SGL_CUR(rctx);
	glGetShaderPrecisionFormat(shader, precision, range, prec);
}

void
sgl_ReleaseShaderCompilerOES(void *rctx)
{
	(void)rctx;
}

void
sgl_ShaderBinaryOES(void *rctx, GLsizei n, const GLuint *shaders, GLenum format, const void *binary,
    GLsizei length)
{
	SGL_CUR(rctx);
	glShaderBinary(n, shaders, format, binary, length);
}

void
sgl_GetIntegerIndexedv(void *rctx, GLenum pname, GLuint index, GLint *data)
{
	SGL_CUR(rctx);
	glGetIntegeri_v(pname, index, data);
}

const GLubyte *
sgl_GetString(void *rctx, GLenum name)
{
	SGL_CUR(rctx);
	const GLubyte *s = glGetString(name);
	/*
	 * Without framebuffer fetch CA's GL renderer drops every layer with a
	 * blend-mode compositing filter (plusD/plusL, colorDodge, multiply,
	 * overlay...: CC toggle circles, App Library plates, Reduce
	 * Transparency backgrounds). Emulated below (fbfetch_*).
	 */
	if (name == GL_EXTENSIONS && s != NULL) {
		static char *ext;
		if (ext == NULL && asprintf(&ext, "%s GL_EXT_shader_framebuffer_fetch", s) < 0) {
			ext = NULL;
		}
		return ext != NULL ? (const GLubyte *)ext : s;
	}
	if (s == NULL) {
		sweet_drm_log("sweetgl: glGetString(%#x) = NULL (error %#x)", name, glGetError());
	}
	return s;
}

/*
 * GL_TEXTURE_RECTANGLE (Apple's ES has it, Mesa's ES does not): such
 * textures are 2D ones (SGL_TGT), and shaders sampling them as
 * sampler2DRect get texel coordinates scaled by sgl_rect_scale, set before
 * each draw to 1/size of the texture on that sampler's unit.
 * ponytail: one rect sampler per shader (CA's), small fixed tables.
 */
#define RECT_MAX 64
static struct { GLuint shader; char sampler[32]; } rect_shaders[RECT_MAX];
static struct { GLuint program; GLint scale, sampler; } rect_programs[RECT_MAX];
static int n_rect_shaders, n_rect_programs;

/* level-0 sizes of 2D textures as created through us (rect scales without GL queries) */
#define TEXSZ_MAX 8192
static uint16_t tex_w[TEXSZ_MAX], tex_h[TEXSZ_MAX];

static void
tex_size_bound(GLenum target, GLint level, GLsizei w, GLsizei h)
{
	GLint tex = 0;
	if (level != 0 || (target != GL_TEXTURE_2D && target != 0x84F5)) {
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	if (tex > 0 && tex < TEXSZ_MAX) {
		tex_w[tex] = (uint16_t)w;
		tex_h[tex] = (uint16_t)h;
	}
}

/*
 * GL_EXT_shader_framebuffer_fetch on a GPU without it: gl_LastFragData[0]
 * becomes a read of the framebuffer's own texture at the fragment, bound
 * to FBFETCH_UNIT before each draw after a flush (texture barrier). Reads
 * see the framebuffer as it was before the draw, not earlier primitives of
 * the same draw. ponytail: CA's draws are quads that do not overlap themselves.
 */
#define FBFETCH_UNIT 15
#define FBFETCH_MAX 512
static GLuint fbfetch_shaders[FBFETCH_MAX];
static int n_fbfetch_shaders;
static const char fbfetch_decl[] =
    "uniform lowp sampler2D sgl_fb;\n"
    "uniform highp vec2 sgl_fb_scale;\n";
static const char fbfetch_read[] = "texture2D(sgl_fb, gl_FragCoord.xy * sgl_fb_scale)";

/* src without the extension, sgl_fb declared after the directives, reads replaced (src is freed) */
static char *
fbfetch_rewrite(char *src)
{
	char *ext = strstr(src, "#extension GL_EXT_shader_framebuffer_fetch");
	for (char *e = ext; e != NULL && *e != '\n' && *e != 0; e++) {
		*e = ' ';
	}
	/* after the leading preprocessor lines */
	size_t at = 0;
	while (src[at] == '#' || src[at] == ' ' || src[at] == '\n') {
		char *nl = strchr(src + at, '\n');
		if (nl == NULL) {
			break;
		}
		at = (size_t)(nl - src + 1);
	}
	size_t reads = 0;
	for (char *r = strstr(src, "gl_LastFragData[0]"); r != NULL; r = strstr(r + 1, "gl_LastFragData[0]")) {
		reads++;
	}
	size_t n = strlen(src);
	char *out = malloc(n + sizeof(fbfetch_decl) + reads * sizeof(fbfetch_read) + 1), *o = out;
	memcpy(o, src, at);
	o += at;
	memcpy(o, fbfetch_decl, sizeof(fbfetch_decl) - 1);
	o += sizeof(fbfetch_decl) - 1;
	for (const char *p = src + at; *p != 0;) {
		if (strncmp(p, "gl_LastFragData[0]", 18) == 0) {
			memcpy(o, fbfetch_read, sizeof(fbfetch_read) - 1);
			o += sizeof(fbfetch_read) - 1;
			p += 18;
		} else {
			*o++ = *p++;
		}
	}
	*o = 0;
	free(src);
	return out;
}

/*
 * Texture functions CA's GL renderer has no tex_fragment_shaders entry for
 * (create_shader fails, the draw is dropped); sgl_install_texfns() points
 * the entries at these calls and the functions are added here. Both use
 * color_matrix[0..3] (input r, g, b, a columns) + [4] (bias).
 * 0x2d (render_vibrant_color_matrix: Control Center off circles, App
 * Library plates): the SW renderer's tex_vibrant_color_matrix, i.e. the
 * destination through the matrix (unpremultiplied) mixed in by the
 * source's alpha.
 * 0x14 (color_matrix_function for a matrix filter without its premultiply
 * flag, next to 0x13 colorMatrix4x4Bias): the matrix on the premultiplied
 * colour as is (CA asks for the full rect then: the bias lights up
 * transparent pixels). SW has neither (no-op there).
 */
static const char vcm_func[] =
    "lowp vec4 sgl_vcm(lowp vec4 s) {\n"
    "  lowp vec4 d = gl_LastFragData[0];\n"
    "  lowp vec4 u = vec4(d.rgb / max(d.a, .005), d.a);\n"
    "  u = clamp(u.r * color_matrix[0] + u.g * color_matrix[1] + u.b * color_matrix[2]\n"
    "    + u.a * color_matrix[3] + color_matrix[4], 0., 1.);\n"
    "  u.rgb *= u.a;\n"
    "  return mix(d, d.a > 0. ? u : vec4(0.), s.a);\n"
    "}\n";
static const char cm4_func[] =
    "lowp vec4 sgl_cm4(lowp vec4 s) {\n"
    "  lowp vec4 u = clamp(s.r * color_matrix[0] + s.g * color_matrix[1] + s.b * color_matrix[2]\n"
    "    + s.a * color_matrix[3] + color_matrix[4], 0., 1.);\n"
    "  return vec4(min(u.rgb, u.a), u.a);\n"
    "}\n";

/*
 * 0x26/0x27 (Context::variable_masked_blur_surface, CAFilter variableBlur):
 * CA copies the source into a surface S, renders a mip chain of S with
 * 0x26 passes and blurs by sampling S's mips per the mask in 0x27. GLES
 * surfaces have one level (set_surface attaches level 0), so each 0x26
 * pass would draw a half-size copy into S itself: they draw nothing here
 * (sgl_nop). 0x27 blurs S in one pass instead: a 24-tap golden-angle disc,
 * Gaussian weighted, radius = mask alpha (1 - alpha if minmax_dir.y, the
 * BlurParams 0x10 flag) times minmax_dir.x (BlurParams radius * 2.6, in
 * pixels) / 2, tap spacing from the texcoord derivatives (S is about
 * destination sized). ponytail: one pass, cost grows with radius only via
 * texture cache misses; a downsampled pre-pass if large blurs get slow.
 */
static const char nop_func[] =
    "lowp vec4 sgl_nop() {\n"
    "  discard;\n"
    "  return vec4(0.);\n"
    "}\n";
static const char vblur_func[] =
    "uniform highp vec4 minmax_dir;\n"
    "lowp vec4 sgl_vblur(lowp vec4 m) {\n"
    "  highp vec2 tc = texcoord0_0;\n"
    "  highp vec2 px = vec2(length(vec2(dFdx(tc.x), dFdy(tc.x))), length(vec2(dFdx(tc.y), dFdy(tc.y))));\n"
    "  mediump float r = (minmax_dir.y > .5 ? 1. - m.a : m.a) * minmax_dir.x * .5;\n"
    "  mediump vec4 acc = texture2D(texture0, tc);\n"
    "  if (r < .5) {\n"
    "    return acc;\n"
    "  }\n"
    "  mediump float wsum = 1.;\n"
    "  for (int i = 1; i <= 24; i++) {\n"
    "    mediump float f = float(i) / 24.;\n"
    "    mediump float ang = float(i) * 2.3999632;\n"
    "    mediump float w = exp(-2. * f);\n"
    "    acc += w * texture2D(texture0, tc + vec2(cos(ang), sin(ang)) * (sqrt(f) * r) * px);\n"
    "    wsum += w;\n"
    "  }\n"
    "  return acc / wsum;\n"
    "}\n";

/*
 * The sgl_ texture functions src calls (and color_matrix if undeclared)
 * before main, extension directives after the leading ones (src is freed).
 */
static char *
texfn_rewrite(char *src)
{
	char *at = strstr(src, "void main");
	if (at == NULL) {
		return src;
	}
	bool cm = strstr(src, "sgl_vcm(") || strstr(src, "sgl_cm4(");
	const char *ext = strstr(src, "sgl_vblur(") ? "#extension GL_OES_standard_derivatives : enable\n" : "";
	const char *parts[5] = {
	    cm && !strstr(src, " color_matrix[5];") ? "uniform lowp vec4 color_matrix[5];\n" : "",
	    strstr(src, "sgl_vcm(") ? vcm_func : "", strstr(src, "sgl_cm4(") ? cm4_func : "",
	    strstr(src, "sgl_nop(") ? nop_func : "", strstr(src, "sgl_vblur(") ? vblur_func : ""};
	/* after the leading preprocessor lines */
	size_t top = 0;
	while (src[top] == '#' || src[top] == ' ' || src[top] == '\n') {
		char *nl = strchr(src + top, '\n');
		if (nl == NULL) {
			break;
		}
		top = (size_t)(nl - src + 1);
	}
	size_t n = strlen(src), pre = (size_t)(at - src), add = strlen(ext);
	for (int i = 0; i < 5; i++) {
		add += strlen(parts[i]);
	}
	char *out = malloc(n + add + 1), *o = out;
	memcpy(o, src, top);
	o += top;
	memcpy(o, ext, strlen(ext));
	o += strlen(ext);
	memcpy(o, src + top, pre - top);
	o += pre - top;
	for (int i = 0; i < 5; i++) {
		memcpy(o, parts[i], strlen(parts[i]));
		o += strlen(parts[i]);
	}
	memcpy(o, at, n - pre + 1);
	free(src);
	return out;
}

static const char rect_prelude[] =
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture2D(s, (c) * sgl_rect_scale)\n"
    "uniform highp vec2 sgl_rect_scale;\n";

void
sgl_ShaderSourceARB(void *rctx, GLhandleARB shader, GLsizei count, const GLcharARB **string, const GLint *length)
{
	SGL_CUR(rctx);
	size_t n = 0;
	for (GLsizei i = 0; i < count; i++) {
		n += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
	}
	char *src = malloc(n + sizeof(rect_prelude) + 1), *p = src;
	for (GLsizei i = 0; i < count; i++) {
		size_t l = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
		memcpy(p, string[i], l);
		p += l;
	}
	*p = 0;
	/*
	 * CA's inverted-mask shader uses coord_arg (clamp bounds) without
	 * declaring it; Apple's compiler takes that, Mesa's does not.
	 */
	const char *extra = strstr(src, "coord_arg") && !strstr(src, "vec4 coord_arg") ?
	    "uniform highp vec4 coord_arg;\n" : "";
	size_t extra_len = strlen(extra);
	if (extra_len) {
		src = realloc(src, n + sizeof(rect_prelude) + extra_len + 1);
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + extra_len, src + at, n - at + 1);
		memcpy(src + at, extra, extra_len);
		n += extra_len;
	}
	const char *rect = strstr(src, "sampler2DRect");
	if (rect != NULL && n_rect_shaders < RECT_MAX) {
		const char *name = rect + strlen("sampler2DRect");
		while (*name == ' ' || *name == '\t') {
			name++;
		}
		size_t l = strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		rect_shaders[n_rect_shaders].shader = H(shader);
		snprintf(rect_shaders[n_rect_shaders].sampler, sizeof(rect_shaders[0].sampler), "%.*s", (int)l, name);
		n_rect_shaders++;
		/* after a leading #version line, if any */
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + sizeof(rect_prelude) - 1, src + at, n - at + 1);
		memcpy(src + at, rect_prelude, sizeof(rect_prelude) - 1);
	}
	if (strstr(src, "sgl_vcm(") != NULL || strstr(src, "sgl_cm4(") != NULL || strstr(src, "sgl_nop(") != NULL ||
	    strstr(src, "sgl_vblur(") != NULL) {
		src = texfn_rewrite(src);
	}
	if (strstr(src, "GL_EXT_shader_framebuffer_fetch") != NULL || strstr(src, "gl_LastFragData") != NULL) {
		src = fbfetch_rewrite(src);
		if (n_fbfetch_shaders < FBFETCH_MAX) {
			fbfetch_shaders[n_fbfetch_shaders++] = H(shader);
		}
	}
	if (dump_shaders) {
		/* whole source, in 400-byte pieces (log lines are 512 bytes) */
		sweet_drm_log("sweetgl: shader %u source (%zu bytes):", H(shader), strlen(src));
		for (size_t i = 0, l = strlen(src); i < l; i += 400) {
			sweet_drm_log("sweetgl:   | %.400s", src + i);
		}
	}
	const GLchar *one = src;
	glShaderSource(H(shader), 1, &one, NULL);
	free(src);
}

static void
rect_link(GLuint program)
{
	GLuint attached[8];
	GLsizei na = 0;
	glGetAttachedShaders(program, 8, &na, attached);
	for (GLsizei a = 0; a < na; a++) {
		for (int i = 0; i < n_rect_shaders; i++) {
			if (rect_shaders[i].shader == attached[a] && n_rect_programs < RECT_MAX) {
				rect_programs[n_rect_programs].program = program;
				rect_programs[n_rect_programs].scale = glGetUniformLocation(program, "sgl_rect_scale");
				rect_programs[n_rect_programs].sampler = glGetUniformLocation(program, rect_shaders[i].sampler);
				n_rect_programs++;
				return;
			}
		}
	}
}

/* per unit: the 2D and rect bindings CA made, and the one GL holds */
#define UNITS 16
static GLuint unit_2d[UNITS], unit_rect[UNITS], unit_held[UNITS];

static int
active_unit(void)
{
	GLint active = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	return (active - GL_TEXTURE0) & (UNITS - 1);
}

GLenum
sgl_tgt(GLenum target)
{
	int u = active_unit();
	GLuint want = target == GL_TEXTURE_2D ? unit_2d[u] : unit_rect[u];
	if (unit_held[u] != want) {
		glBindTexture(GL_TEXTURE_2D, want);
		unit_held[u] = want;
	}
	return GL_TEXTURE_2D;
}

/*
 * Textures holding a copy of a surface (copy_surface): copied again before
 * a draw when the surface's seed moved (its writer unlocked it since).
 * ponytail: one copy per texture name below TEXSZ_MAX.
 */
struct copied {
	IOSurfaceRef surface;
	uint32_t seed;
	GLenum format, type;
};
static struct copied copies[TEXSZ_MAX];
/* texture swizzle of the bound texture: identity, or A,B,G,R bytes read as RGBA */
static uint8_t tex_argb[TEXSZ_MAX];
static bool copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h,
    size_t pitch, const char *why);
static void set_swizzle(GLenum target, bool argb);

static void
copy_forget(GLuint tex)
{
	if (tex < TEXSZ_MAX && copies[tex].surface != NULL) {
		CFRelease(copies[tex].surface);
		copies[tex].surface = NULL;
	}
}

static void
copy_refresh(int u, int *active)
{
	GLuint tex = unit_held[u];
	struct copied *c = tex < TEXSZ_MAX ? &copies[tex] : NULL;
	if (c == NULL || c->surface == NULL || IOSurfaceGetSeed(c->surface) == c->seed) {
		return;
	}
	if (*active < 0) {
		*active = active_unit();
	}
	glActiveTexture(GL_TEXTURE0 + u);
	IOSurfaceRef s = c->surface;
	copy_surface(s, GL_TEXTURE_2D, c->format, c->type, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
	    IOSurfaceGetBytesPerRow(s), "refresh");
}

/*
 * SWEETGL_TRACE: samplers of the current program whose texture has no level
 * 0 (unbound or never filled), once per program/unit/texture.
 */
static void
trace_empty_samplers(GLint prog)
{
	static uint32_t seen[4096];
	GLint n = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
	for (GLint i = 0; i < n; i++) {
		char name[64];
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &type, name);
		if (type != GL_SAMPLER_2D) {
			continue;
		}
		GLint unit = 0, tex = 0, w = 0, active = GL_TEXTURE0;
		glGetUniformiv(prog, glGetUniformLocation(prog, name), &unit);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glActiveTexture(active);
		uint32_t key = (uint32_t)prog << 20 ^ (uint32_t)unit << 16 ^ (uint32_t)tex;
		if (w > 0 || seen[key % 4096] == key + 1) {
			continue;
		}
		seen[key % 4096] = key + 1;
		TRACE("empty sampler: program %d %s unit %d tex %d", prog, name, unit, tex);
	}
}

extern void _mesa_TextureBarrierNV(void);

/*
 * CA reads the destination by sampling the texture it is rendering into
 * (Apple's GPUs return what memory held when the pass began). A flush
 * first makes the earlier draws visible to the sampler; without it the
 * read came back empty (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: sampler units per program read once (CA sets them at link).
 */
#define PROG_MAX 4096
static uint16_t prog_units[PROG_MAX];   /* bit u: a sampler reads unit u; bit 15: known */

static void
feedback_barrier(GLint prog)
{
	GLint fb = 0, att = 0, type = 0;
	if (prog <= 0 || prog >= PROG_MAX) {
		return;
	}
	if (!(prog_units[prog] & 0x8000)) {
		uint16_t m = 0x8000;
		GLint n = 0;
		glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
		for (GLint i = 0; i < n; i++) {
			char name[64];
			GLint size = 0, unit = 0;
			GLenum t = 0;
			glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &t, name);
			if (t == GL_SAMPLER_2D) {
				glGetUniformiv(prog, glGetUniformLocation(prog, name), &unit);
				m |= unit >= 0 && unit < 15 ? 1u << unit : 0;
			}
		}
		prog_units[prog] = m;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type != GL_TEXTURE) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	for (int u = 0; u < 15; u++) {
		if ((prog_units[prog] & 1u << u) && unit_held[u] == (GLuint)att) {
			WTRACE("feedback: prog %d samples fb %d's texture %d on unit %d: barrier", prog, fb, att, u);
			_mesa_TextureBarrierNV();
			return;
		}
	}
}

/* SWEETGL_TRACE: whether this draw is in a window (opens one when due) */
static bool
window_draw(void)
{
	struct timespec t;
	GLint vp[4] = {0};
	if (trace_left <= 0) {
		return false;
	}
	if (win_left > 0 && --win_left > 0) {
		return true;
	}
	glGetIntegerv(GL_VIEWPORT, vp);
	if (!WIN_TRIGGER(vp[2], vp[3])) {
		return false;
	}
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t.tv_sec < win_next) {
		return false;
	}
	win_next = t.tv_sec + WIN_SECS;
	win_left = WIN_DRAWS;
	TRACE("window at %lds (%dx%d draw): next %d draws", (long)t.tv_sec, vp[2], vp[3], WIN_DRAWS);
	return true;
}

/* SWEETGL_TRACE: the draw's state and the program's non-sampler uniforms */
static void
trace_draw_state(GLint prog, GLenum mode, GLsizei count)
{
	GLint v[16] = {0};
	GLboolean m[4] = {0};
	v[0] = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &v[1]);
	glGetIntegerv(GL_BLEND_DST_RGB, &v[2]);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &v[3]);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &v[4]);
	v[10] = glIsEnabled(GL_SCISSOR_TEST);
	glGetBooleanv(GL_COLOR_WRITEMASK, m);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v[11]);
	GLint vp[4] = {0}, sc[4] = {0};
	glGetIntegerv(GL_VIEWPORT, vp);
	glGetIntegerv(GL_SCISSOR_BOX, sc);
	char samp[64] = "";
	for (int u = 0, k = 0; u < 15 && prog < PROG_MAX && k < 56; u++) {
		if (prog_units[prog] & 1u << u) {
			k += snprintf(samp + k, sizeof(samp) - (size_t)k, " u%d=%u", u, unit_held[u]);
		}
	}
	GLint fbtex = 0;
	if (v[11] != 0) {
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &fbtex);
	}
	TRACE("draw prog %d samples%s into %d tex %u %u %u mode %#x n %d fb %d vp %d,%d %dx%d blend %d %#x/%#x %#x/%#x scissor %d %d,%d %dx%d "
	    "mask %d%d%d%d", prog, samp, fbtex, unit_held[0], unit_held[1], unit_held[2], mode, count, v[11], vp[0], vp[1], vp[2], vp[3],
	    v[0], v[1], v[2], v[3], v[4], v[10], sc[0], sc[1], sc[2], sc[3], m[0], m[1], m[2], m[3]);
	char line[400];
	size_t len = 0;
	GLint n = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
	for (GLint i = 0; i < n && len < sizeof(line) - 80; i++) {
		char name[64];
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &type, name);
		int k = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3 :
		    type == GL_FLOAT_VEC4 ? 4 : type == GL_FLOAT_MAT4 ? 16 : 0;
		if (k == 0 || strncmp(name, "texmat", 6) == 0 || strcmp(name, "mvp") == 0) {
			continue;
		}
		char *br = strchr(name, '[');
		if (br != NULL) {
			*br = 0;
		}
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s=", name);
		for (GLint e = 0; e < size && e < 8; e++) {
			char el[80];
			GLfloat f[16] = {0};
			snprintf(el, sizeof(el), size > 1 ? "%s[%d]" : "%s", name, e);
			glGetUniformfv(prog, glGetUniformLocation(prog, el), f);
			for (int j = 0; j < k && len < sizeof(line) - 12; j++) {
				len += (size_t)snprintf(line + len, sizeof(line) - len, "%s%.3g", j ? "," : e ? ";" : "", f[j]);
			}
		}
	}
	if (len > 0) {
		TRACE("  uniforms%s", line);
	}
}

/* SWEETGL_TRACE: mean RGBA and a 24x4 luma thumbnail of the scissor box (or viewport) */
static void
trace_region(const char *when)
{
	GLint vp[4] = {0}, r[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	memcpy(r, vp, sizeof(r));
	if (glIsEnabled(GL_SCISSOR_TEST)) {
		GLint sc[4] = {0};
		glGetIntegerv(GL_SCISSOR_BOX, sc);
		GLint x0 = sc[0] > vp[0] ? sc[0] : vp[0], y0 = sc[1] > vp[1] ? sc[1] : vp[1];
		GLint x1 = sc[0] + sc[2] < vp[0] + vp[2] ? sc[0] + sc[2] : vp[0] + vp[2];
		GLint y1 = sc[1] + sc[3] < vp[1] + vp[3] ? sc[1] + sc[3] : vp[1] + vp[3];
		r[0] = x0, r[1] = y0, r[2] = x1 - x0, r[3] = y1 - y0;
	}
	if (r[2] <= 0 || r[3] <= 0 || (size_t)r[2] * (size_t)r[3] > 250000) {
		TRACE("  %s %d,%d %dx%d", when, r[0], r[1], r[2], r[3]);
		return;
	}
	size_t w = (size_t)r[2], h = (size_t)r[3];
	uint8_t *px = malloc(w * h * 4);
	if (px == NULL) {
		return;
	}
	glReadPixels(r[0], r[1], r[2], r[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
	uint64_t sum[4] = {0};
	for (size_t i = 0; i < w * h * 4; i++) {
		sum[i % 4] += px[i];
	}
	char thumb[4 * 25 + 1];
	for (int cy = 0; cy < 4; cy++) {
		for (int cx = 0; cx < 24; cx++) {
			size_t x0 = w * cx / 24, x1 = w * (cx + 1) / 24, y0 = h * (3 - cy) / 4, y1 = h * (4 - cy) / 4;
			uint64_t a = 0, l = 0, cnt = 0;
			for (size_t y = y0; y < y1; y++) {
				for (size_t x = x0; x < x1; x++) {
					const uint8_t *q = px + (y * w + x) * 4;
					a += q[3];
					l += (q[0] * 54u + q[1] * 183u + q[2] * 19u) >> 8;
					cnt++;
				}
			}
			thumb[cy * 25 + cx] = cnt == 0 || a == 0 ? ' ' : (char)('0' + l * 9 / (cnt * 255));
		}
		thumb[cy * 25 + 24] = '/';
	}
	thumb[4 * 25 - 1] = 0;
	size_t n = w * h;
	TRACE("  %s %d,%d %dx%d mean %02x%02x%02x%02x |%s|", when, r[0], r[1], r[2], r[3], (unsigned)(sum[0] / n),
	    (unsigned)(sum[1] / n), (unsigned)(sum[2] / n), (unsigned)(sum[3] / n), thumb);
	free(px);
}

/* after a new draw state (trace): what the viewport now holds */
static void
trace_draw_result(GLint prog)
{
	(void)prog;
	trace_region("after");
}

/* SWEETGL_TRACE: an attribute of the draw's first vertices (client arrays) or its constant value */
static void
trace_attrib(GLint prog, const char *name, GLenum itype, const void *indices, GLint first)
{
	GLint loc = glGetAttribLocation(prog, name), on = 0;
	if (loc < 0) {
		return;
	}
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
	if (!on) {
		GLfloat f[4] = {0};
		glGetVertexAttribfv(loc, GL_CURRENT_VERTEX_ATTRIB, f);
		TRACE("  %s constant %.3g,%.3g,%.3g,%.3g", name, f[0], f[1], f[2], f[3]);
		return;
	}
	GLint size = 0, type = 0, stride = 0, norm = 0, buf = 0;
	void *ptr = NULL;
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
	glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
	glGetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
	char line[480];
	size_t len = (size_t)snprintf(line, sizeof(line), "  %s size %d type %#x norm %d stride %d buf %d ptr %p:", name, size,
	    type, norm, stride, buf, ptr);
	for (int v = 0; v < 8 && ptr != NULL && buf == 0; v++) {
		GLint idx = first + v;
		if (indices != NULL) {
			idx = itype == GL_UNSIGNED_SHORT ? ((const GLushort *)indices)[v] :
			      itype == GL_UNSIGNED_BYTE ? ((const GLubyte *)indices)[v] : (GLint)((const GLuint *)indices)[v];
		}
		int esz = type == GL_FLOAT ? 4 : type == GL_UNSIGNED_SHORT || type == GL_SHORT || type == GL_HALF_FLOAT ? 2 : 1;
		const uint8_t *e = (const uint8_t *)ptr + (size_t)idx * (size_t)(stride ? stride : size * esz);
		len += (size_t)snprintf(line + len, sizeof(line) - len, " [%d]", idx);
		for (int c = 0; c < size && len < sizeof(line) - 16; c++) {
			if (type == GL_FLOAT) {
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %.4g", ((const float *)e)[c]);
			} else if (type == GL_HALF_FLOAT) {
				__fp16 h;
				memcpy(&h, e + 2 * c, 2);
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %.4g", (double)h);
			} else if (esz == 2) {
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %04x", ((const uint16_t *)e)[c]);
			} else {
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %02x", e[c]);
			}
		}
	}
	TRACE("%s", line);
}

static void
trace_vertex_color(GLint prog, GLenum itype, const void *indices, GLint first)
{
	trace_attrib(prog, "vertex_position", itype, indices, first);
	trace_attrib(prog, "vertex_color", itype, indices, first);
	trace_attrib(prog, "vertex_texcoord0", itype, indices, first);
	GLfloat t[4] = {0};
	GLint loc = glGetUniformLocation(prog, "texmat0");
	if (loc >= 0) {
		glGetUniformfv(prog, loc, t);
		TRACE("  texmat0 %.4g,%.4g,%.4g,%.4g", t[0], t[1], t[2], t[3]);
	}
}

static void trace_region(const char *when);
static void trace_draw_state(GLint prog, GLenum mode, GLsizei count);

/* sampler and scale locations + 1 of programs with an fbfetch shader (0: none) */
static GLint fbfetch_sampler[PROG_MAX], fbfetch_scale[PROG_MAX];
static bool fbfetch_pending;    /* trace the region after this draw */

static void
fbfetch_link(GLuint program)
{
	GLuint attached[8];
	GLsizei na = 0;
	if (program >= PROG_MAX) {
		return;
	}
	fbfetch_sampler[program] = fbfetch_scale[program] = 0;
	glGetAttachedShaders(program, 8, &na, attached);
	for (GLsizei a = 0; a < na; a++) {
		for (int i = 0; i < n_fbfetch_shaders; i++) {
			if (fbfetch_shaders[i] == attached[a]) {
				fbfetch_sampler[program] = glGetUniformLocation(program, "sgl_fb") + 1;
				fbfetch_scale[program] = glGetUniformLocation(program, "sgl_fb_scale") + 1;
				return;
			}
		}
	}
}

/* before a draw of an fbfetch program: the framebuffer's texture on FBFETCH_UNIT, flushed */
static void
fbfetch_bind(GLint prog)
{
	GLint fb = 0, type = 0, att = 0, w = 0, h = 0, active = GL_TEXTURE0;
	if (prog <= 0 || prog >= PROG_MAX || fbfetch_sampler[prog] == 0) {
		return;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	if (type != GL_TEXTURE || att <= 0) {
		return;
	}
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0 + FBFETCH_UNIT);
	glBindTexture(GL_TEXTURE_2D, (GLuint)att);
	unit_held[FBFETCH_UNIT] = (GLuint)att;
	if (att < TEXSZ_MAX && tex_w[att] > 0) {
		w = tex_w[att];
		h = tex_h[att];
	} else {
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	}
	glActiveTexture((GLenum)active);
	glUniform1i(fbfetch_sampler[prog] - 1, FBFETCH_UNIT);
	if (fbfetch_scale[prog] > 0) {
		glUniform2f(fbfetch_scale[prog] - 1, w > 0 ? 1.0f / (GLfloat)w : 1.0f, h > 0 ? 1.0f / (GLfloat)h : 1.0f);
	}
	_mesa_TextureBarrierNV();
	/* SWEETGL_TRACE: the first draws of each fbfetch program, wherever they happen */
	static uint8_t traced[PROG_MAX];
	if (trace_left > 0 && win_left == 0 && traced[prog] < 12) {
		traced[prog]++;
		TRACE("fbfetch draw %d of prog %d: fb %d texture %d %dx%d", traced[prog], prog, fb, att, w, h);
		trace_draw_state(prog, 0, 0);
		trace_region("before");
		fbfetch_pending = true;
	}
}

/* before a draw: each unit holds what the program samples there, rect scale set */
static bool
draw_prep(GLenum mode, GLsizei count)
{
	GLint prog = 0, unit = -1, w = 0, h = 0, scale = -1;
	bool fresh = false;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	for (int i = 0; i < n_rect_programs; i++) {
		if (rect_programs[i].program == (GLuint)prog) {
			scale = rect_programs[i].scale;
			unit = 0;
			if (rect_programs[i].sampler >= 0) {
				glGetUniformiv(prog, rect_programs[i].sampler, &unit);
			}
			break;
		}
	}
	int active = -1;
	for (int u = 0; u < UNITS; u++) {
		GLuint want = u == unit ? unit_rect[u] : unit_2d[u];
		if (unit_held[u] != want) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + u);
			glBindTexture(GL_TEXTURE_2D, want);
			unit_held[u] = want;
		}
		if (want != 0) {
			copy_refresh(u, &active);
		}
	}
	if (unit >= 0 && unit < UNITS && scale >= 0) {
		GLuint tex = unit_rect[unit];
		if (tex < TEXSZ_MAX) {
			w = tex_w[tex];
			h = tex_h[tex];
		}
		if (w <= 0 || h <= 0) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + unit);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		}
		glUniform2f(scale, w > 0 ? 1.0f / w : 1.0f, h > 0 ? 1.0f / h : 1.0f);
	}
	if (active >= 0) {
		glActiveTexture(GL_TEXTURE0 + active);
	}
	fbfetch_bind(prog);
	feedback_barrier(prog);
	if (prog != 0 && window_draw()) {
		trace_empty_samplers(prog);
		trace_draw_state(prog, mode, count);
		trace_region("before");
		fresh = true;
	}
	return fresh;
}

void
sgl_BindTexture(void *rctx, GLenum target, GLuint texture)
{
	SGL_CUR(rctx);
	if (target == GL_TEXTURE_2D || target == 0x84F5) {
		int u = active_unit();
		*(target == GL_TEXTURE_2D ? &unit_2d[u] : &unit_rect[u]) = texture;
		unit_held[u] = texture;
		glBindTexture(GL_TEXTURE_2D, texture);
	} else {
		glBindTexture(target, texture);
	}
	SGL_POST(5);
}

/* GL unbinds deleted textures everywhere: so do the shadows */
void
sgl_DeleteTextures(void *rctx, GLsizei n, const GLuint *textures)
{
	SGL_CUR(rctx);
	for (GLsizei i = 0; i < n; i++) {
		copy_forget(textures[i]);
		if (textures[i] < TEXSZ_MAX) {
			tex_argb[textures[i]] = 0;      /* a reused name starts unswizzled */
		}
		for (int u = 0; u < UNITS; u++) {
			if (unit_2d[u] == textures[i]) {
				unit_2d[u] = 0;
			}
			if (unit_rect[u] == textures[i]) {
				unit_rect[u] = 0;
			}
			if (unit_held[u] == textures[i]) {
				unit_held[u] = 0;
			}
		}
	}
	glDeleteTextures(n, textures);
	SGL_POST(59);
}

void
sgl_PixelStorei(void *rctx, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	(void)0;
	glPixelStorei(pname, param);
	SGL_POST(195);
}

void
sgl_DrawArrays(void *rctx, GLenum mode, GLint first, GLsizei count)
{
	SGL_CUR(rctx);
	bool fresh = draw_prep(mode, count);
	if (fresh) {
		GLint prog = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		trace_vertex_color(prog, 0, NULL, first);
	}
	glDrawArrays(mode, first, count);
	if (fbfetch_pending) {
		fbfetch_pending = false;
		trace_region("after");
	}
	if (fresh) {
		GLint prog = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		trace_draw_result(prog);
	}
	SGL_POST(65);
}

void
sgl_DrawElements(void *rctx, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	SGL_CUR(rctx);
	bool fresh = draw_prep(mode, count);
	if (fresh) {
		GLint prog = 0, ebuf = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
		trace_vertex_color(prog, type, ebuf == 0 ? indices : NULL, 0);
	}
	glDrawElements(mode, count, type, indices);
	if (fbfetch_pending) {
		fbfetch_pending = false;
		trace_region("after");
	}
	if (fresh) {
		GLint prog = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		trace_draw_result(prog);
	}
	SGL_POST(67);
}

/* failures logged with the info log and source: CA does not ask for them */
void
sgl_CompileShaderARB(void *rctx, GLhandleARB shader)
{
	SGL_CUR(rctx);
	glCompileShader(H(shader));
	GLint ok = 0;
	glGetShaderiv(H(shader), GL_COMPILE_STATUS, &ok);
	if (!ok) {
		static char log[1024], src[4096];
		glGetShaderInfoLog(H(shader), sizeof(log), NULL, log);
		glGetShaderSource(H(shader), sizeof(src), NULL, src);
		sweet_drm_log("sweetgl: shader %u compile failed: %.400s", H(shader), log);
		for (size_t i = 0, n = strlen(src); i < n; i += 400) {
			sweet_drm_log("sweetgl:   src %.400s", src + i);
		}
	}
}

void
sgl_LinkProgramARB(void *rctx, GLhandleARB program)
{
	SGL_CUR(rctx);
	glLinkProgram(H(program));
	GLint ok = 0;
	glGetProgramiv(H(program), GL_LINK_STATUS, &ok);
	if (ok) {
		rect_link(H(program));
		fbfetch_link(H(program));
	}
	if (dump_shaders) {
		GLuint sh[4] = {0};
		GLsizei n = 0;
		glGetAttachedShaders(H(program), 4, &n, sh);
		sweet_drm_log("sweetgl: program %u = shaders %u %u", H(program), sh[0], sh[1]);
	}
	if (!ok) {
		static char log[1024];
		glGetProgramInfoLog(H(program), sizeof(log), NULL, log);
		sweet_drm_log("sweetgl: program %u link failed: %.400s", H(program), log);
	}
}

void
sgl_FramebufferTexture2DEXT(void *rctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	TRACE("FramebufferTexture2D fb %d attachment %#x tex %u", fb, attachment, texture);
	glFramebufferTexture2D(target, attachment, textarget == 0x84F5 ? GL_TEXTURE_2D : textarget, texture, level);
	SGL_POST(677);
}

void
sgl_TexParameteri(void *rctx, GLenum target, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	if (pname >= 0x8E42 && pname <= 0x8E46) {
		TRACE("TexParameteri tex %u swizzle %#x = %#x", unit_held[active_unit()], pname, param);
	}
	glTexParameteri(target, pname, param);
	SGL_POST(304);
}

void
sgl_Clear(void *rctx, GLbitfield mask)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	GLfloat cc[4] = {0};
	GLint sc[4] = {0};
	glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
	glGetIntegerv(GL_SCISSOR_BOX, sc);
	WTRACE("Clear fb %d mask %#x color %.3g,%.3g,%.3g,%.3g scissor %d %d,%d %dx%d", fb, mask, cc[0], cc[1], cc[2], cc[3],
	    glIsEnabled(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3]);
	glClear(mask);
	SGL_POST(10);
}

void
sgl_BindFramebufferEXT(void *rctx, GLenum target, GLuint framebuffer)
{
	SGL_CUR(rctx);
	WTRACE("BindFramebuffer %u", framebuffer);
	glBindFramebuffer(target, framebuffer);
	SGL_POST(672);
}

static GLint
unpack_row(void)
{
	GLint row = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	return row;
}

void
sgl_TexSubImage2D(void *rctx, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	TRACE("TexSubImage2D tex %u level %d at %d,%d %dx%d fmt %#x type %#x row %d", unit_held[active_unit()],
	    level, x, y, width, height, format, type, unpack_row());
	glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
	SGL_POST(307);
}

/* Apple's ES takes internalformat GL_RGBA with format GL_BGRA; Mesa wants GL_BGRA for both */
void
sgl_TexImage2D(void *rctx, GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	GLenum asked = target;
	target = SGL_TGT(target);
	TRACE("TexImage2D target %#x unit %d tex %u level %d ifmt %#x %dx%d fmt %#x type %#x row %d", asked,
	    active_unit(), unit_held[active_unit()], level, internalformat, width, height, format, type, unpack_row());
	copy_forget(unit_held[active_unit()]);
	set_swizzle(target, false);
	if (format == GL_BGRA_EXT && (internalformat == GL_RGBA || internalformat == GL_RGBA8)) {
		internalformat = GL_BGRA_EXT;
	}
	glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
	SGL_POST(301);
	tex_size_bound(target, level, width, height);
}

/*
 * ponytail: flush = finish. CA hands rendered IOSurfaces to the display
 * (IOMFB swap, copied by the kernel at once) without waiting for the GPU;
 * per-surface fences in the swap path would let the GPU run ahead.
 */
/*
 * Frame timing, logged every 10 s: flushes, time blocked in glFinish (GPU
 * still busy) and the CPU time between one flush's return and the next.
 */
static void
flush_stats(uint64_t t0, uint64_t t1)
{
	static uint64_t last, window, n, wait, wait_max, cpu;
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	if (last != 0) {
		cpu += t0 - last;
	}
	wait += t1 - t0;
	wait_max = t1 - t0 > wait_max ? t1 - t0 : wait_max;
	n++;
	last = t1;
	if (window == 0) {
		window = t1;
	}
	uint64_t span = (t1 - window) * tb.numer / tb.denom;
	if (span >= 10000000000ull) {
		sweet_drm_log("sweetgl: %.1f flushes/s, finish avg %llu us max %llu us, between %llu us",
		    n * 1e9 / span, wait * tb.numer / tb.denom / 1000 / n, wait_max * tb.numer / tb.denom / 1000,
		    cpu * tb.numer / tb.denom / 1000 / n);
		window = t1;
		n = wait = wait_max = cpu = 0;
	}
}

void
sgl_Flush(void *rctx)
{
	SGL_CUR(rctx);
	uint64_t t0 = mach_absolute_time();
	glFinish();
	flush_stats(t0, mach_absolute_time());
}

void
sgl_FinishObjectAPPLE(void *rctx, GLenum object, GLint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
}

GLboolean
sgl_TestObjectAPPLE(void *rctx, GLenum object, GLuint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
	return GL_TRUE;
}

/* ---- IOSurfaces as textures ---- */

static bool
get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)
{
	(void)fscreen;
	struct sgl_image *img = image;
	pipe_resource_reference(&out->texture, img->res);
	out->format = img->format;
	out->level = 0;
	out->layer = 0;
	out->internalformat = img->internalformat;
	return true;
}

static bool
validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
	(void)fscreen;
	return image != NULL;
}

void
sweetgl_image_hooks(struct pipe_frontend_screen *fscreen)
{
	fscreen->get_egl_image = get_egl_image;
	fscreen->validate_egl_image = validate_egl_image;
}

static enum pipe_format
gl_format(GLenum format, GLenum type, unsigned *internalformat)
{
	bool half = type == GL_HALF_FLOAT || type == 0x8D61 /* GL_HALF_FLOAT_OES */;
	switch (format) {
	case GL_BGRA_EXT:
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case GL_RGBA:
		*internalformat = half ? GL_RGBA16F : GL_RGBA8;
		return half ? PIPE_FORMAT_R16G16B16A16_FLOAT : PIPE_FORMAT_R8G8B8A8_UNORM;
	case GL_LUMINANCE_ALPHA:
		*internalformat = GL_LUMINANCE_ALPHA;
		return PIPE_FORMAT_L8A8_UNORM;
	case GL_LUMINANCE:
		*internalformat = GL_LUMINANCE;
		return PIPE_FORMAT_L8_UNORM;
	case GL_ALPHA:
		*internalformat = GL_ALPHA;
		return PIPE_FORMAT_A8_UNORM;
	case GL_RED:
		*internalformat = half ? GL_R16F : GL_R8;
		return half ? PIPE_FORMAT_R16_FLOAT : PIPE_FORMAT_R8_UNORM;
	case GL_RG:
		*internalformat = half ? GL_RG16F : GL_RG8;
		return half ? PIPE_FORMAT_R16G16_FLOAT : PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

static enum pipe_format
surface_format(uint32_t fourcc, size_t bpe, unsigned *internalformat)
{
	switch (fourcc) {
	case 0:                 /* CA's own render surfaces carry no format */
		return bpe == 4 ? gl_format(GL_BGRA_EXT, GL_UNSIGNED_BYTE, internalformat) :
		       bpe == 8 ? gl_format(GL_RGBA, GL_HALF_FLOAT, internalformat) :
		       bpe == 1 ? gl_format(GL_RED, GL_UNSIGNED_BYTE, internalformat) : PIPE_FORMAT_NONE;
	case 'LA08':
		return gl_format(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'A008':
		return gl_format(GL_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'BGRA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 'RGBA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 'RGhA':
		*internalformat = GL_RGBA16F;
		return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 'L008':
		*internalformat = GL_R8;
		return PIPE_FORMAT_R8_UNORM;
	case 'L016':
		*internalformat = GL_RG8;
		return PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

/* fourcc as 4 printable characters ('.' for the rest) */
#define FCC_C(c) ((c) >= 0x20 && (c) < 0x7f ? (c) : '.')
#define FCC(f) FCC_C((f) >> 24), FCC_C(((f) >> 16) & 0xff), FCC_C(((f) >> 8) & 0xff), FCC_C((f) & 0xff)

/*
 * One Mesa resource per surface (and format): CA renders into a surface
 * through one texture (attachImage) and samples it through another
 * (texImageIOSurface); as separate resources Mesa could run the reading
 * batch before the writing one (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: 32 entries, least recently used dropped; a dropped entry keeps
 * nothing alive, a freed surface's pages stay imported until then.
 */
struct surf_res {
	uint32_t id, w, h, pitch;
	enum pipe_format format;
	uintptr_t base;
	struct pipe_resource *res;
	uint64_t used;
};
#define SURF_RES_MAX 32
static struct surf_res surf_res[SURF_RES_MAX];
static uint64_t surf_res_clock;

static bool
surf_res_same(const struct surf_res *a, const struct surf_res *b)
{
	return a->id == b->id && a->format == b->format && a->w == b->w && a->h == b->h && a->pitch == b->pitch &&
	       a->base == b->base;
}

/* a new reference to the cached resource, or NULL */
static struct pipe_resource *
surf_res_get(const struct surf_res *key)
{
	for (int i = 0; i < SURF_RES_MAX; i++) {
		if (surf_res[i].res != NULL && surf_res_same(&surf_res[i], key)) {
			struct pipe_resource *res = NULL;
			surf_res[i].used = ++surf_res_clock;
			pipe_resource_reference(&res, surf_res[i].res);
			return res;
		}
	}
	return NULL;
}

static void
surf_res_put(const struct surf_res *key, struct pipe_resource *res)
{
	if (res == NULL) {
		return;
	}
	int victim = 0;
	for (int i = 1; i < SURF_RES_MAX; i++) {
		if (surf_res[i].used < surf_res[victim].used) {
			victim = i;
		}
	}
	pipe_resource_reference(&surf_res[victim].res, NULL);
	surf_res[victim] = *key;
	surf_res[victim].used = ++surf_res_clock;
	pipe_resource_reference(&surf_res[victim].res, res);
}


static void
set_swizzle(GLenum target, bool argb)
{
	GLuint tex = unit_held[active_unit()];
	if (tex >= TEXSZ_MAX || tex_argb[tex] == argb) {
		return;
	}
	tex_argb[tex] = argb;
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, argb ? GL_ALPHA : GL_RED);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, argb ? GL_BLUE : GL_GREEN);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, argb ? GL_GREEN : GL_BLUE);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, argb ? GL_RED : GL_ALPHA);
}

/* GL upload format/type of a surface CA gave none for (as surface_format) */
static void
upload_format(uint32_t fourcc, size_t bpe, GLenum *format, GLenum *type)
{
	*type = GL_UNSIGNED_BYTE;
	switch (fourcc) {
	case 0:
		*format = bpe == 4 ? GL_BGRA_EXT : bpe == 8 ? GL_RGBA : bpe == 1 ? GL_RED : 0;
		*type = bpe == 8 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
		break;
	case 'LA08': *format = GL_LUMINANCE_ALPHA; break;
	case 'A008': *format = GL_ALPHA; break;
	case 'BGRA': *format = GL_BGRA_EXT; break;
	case 'RGBA': *format = GL_RGBA; break;
	case 'RGhA': *format = GL_RGBA; *type = GL_HALF_FLOAT; break;
	case 'L008': *format = GL_RED; break;
	case 'L016': *format = GL_RG; break;
	default: *format = 0;
	}
}

/*
 * Rows the GPU cannot sample in place (A5xx pitch is 64-byte aligned; CA's
 * own bitmaps are not): copied into the texture instead.
 * ponytail: a snapshot, later CPU writes to the surface are not seen (CA's
 * images are immutable).
 */
static bool
copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h, size_t pitch,
    const char *why)
{
	size_t bpe = IOSurfaceGetBytesPerElement(s);
	const void *base = IOSurfaceGetBaseAddress(s);
	if (base == NULL || bpe == 0 || pitch % bpe != 0) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: cannot copy", why, IOSurfaceGetID(s), w, h, pitch);
		return false;
	}
	GLint row = 0, align = 4, skip_rows = 0, skip_pixels = 0;
	uint32_t seed = IOSurfaceGetSeed(s);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / bpe));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	GLint ifmt = format == GL_RED ? GL_R8 : format == GL_RG ? GL_RG8 :
	    type == GL_HALF_FLOAT ? GL_RGBA16F : (GLint)format;
	glTexImage2D(target, 0, ifmt, (GLsizei)w, (GLsizei)h, 0, format, type, base);
	GLenum e = glGetError();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	GLuint tex = unit_held[active_unit()];
	if (tex < TEXSZ_MAX) {
		struct copied *c = &copies[tex];
		if (c->surface != s) {
			CFRetain(s);
			copy_forget(tex);
			c->surface = s;
		}
		c->seed = seed;
		c->format = format;
		c->type = type;
	}
	size_t lit = 0, any = 0;
	const uint8_t *b8 = base, *mid = b8 + (h / 2) * pitch;
	for (size_t i = 0; trace_left > 0 && i < pitch * h; i++) {
		lit += i % bpe == bpe - 1 && b8[i] != 0;
		any += b8[i] != 0;
	}
	TRACE("%s surf %u tex %u %zux%zu pitch %zu fmt %#x/%#x copied (seed %u, %zu of %zu last bytes set, %zu bytes set, "
	    "mid row %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x), err %#x",
	    why, IOSurfaceGetID(s), tex, w, h, pitch, format, type, seed, lit, pitch * h / bpe, any,
	    mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7], mid[8], mid[9], mid[10], mid[11],
	    mid[12], mid[13], mid[14], mid[15], e);
	if (e != GL_NO_ERROR) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu fmt %#x/%#x: copy error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, format, type, e);
	}
	return e == GL_NO_ERROR;
}

/* The surface's pages as the storage of the texture bound to target. */
static bool
bind_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, uint32_t plane, const char *why)
{
	static int logged;
	uint32_t fourcc = IOSurfaceGetPixelFormat(s);
	size_t w = IOSurfaceGetWidthOfPlane(s, plane), h = IOSurfaceGetHeightOfPlane(s, plane);
	size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane);
	struct sgl_image img = {0};
	bool multi = IOSurfaceGetPlaneCount(s) > 1 || plane != 0;   /* ponytail: YUV surfaces not yet */
	img.format = format && !multi ? gl_format(format, type, &img.internalformat) : PIPE_FORMAT_NONE;
	if (img.format == PIPE_FORMAT_NONE && !multi) {
		img.format = surface_format(fourcc, IOSurfaceGetBytesPerElementOfPlane(s, plane), &img.internalformat);
	}
	/*
	 * CA's own 32-bit images without an IOSurface format (attached, no GL
	 * format) are A,B,G,R in memory (the Settings "+" circle, systemBlue:
	 * fc fc 79 00) and sampled as RGBA by its shaders.
	 */
	bool argb = format == 0 && fourcc == 0 && !multi && IOSurfaceGetBytesPerElement(s) == 4;
	if (argb) {
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
		img.format = gl_format(format, type, &img.internalformat);
	}
	set_swizzle(target, argb);
	if (!multi && pitch % 64 != 0) {
		if (format == 0) {
			upload_format(fourcc, IOSurfaceGetBytesPerElement(s), &format, &type);
		}
		if (format != 0) {
			return copy_surface(s, target, format, type, w, h, pitch, why);
		}
	}
	uint32_t handle = 0;
	uint64_t size = 0;
	const char *step = img.format == PIPE_FORMAT_NONE ? "format" : "kernel import";
	struct surf_res key = {
		.id = IOSurfaceGetID(s),
		.format = img.format,
		.w = (uint32_t)w,
		.h = (uint32_t)h,
		.pitch = (uint32_t)pitch,
		.base = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane),
	};
	img.res = img.format != PIPE_FORMAT_NONE ? surf_res_get(&key) : NULL;
	if (img.res == NULL && img.format != PIPE_FORMAT_NONE &&
	    sweet_drm_import_iosurface(IOSurfaceGetID(s), (uint64_t)(uintptr_t)IOSurfaceGetBaseAddress(s),
	    &handle, &size) == 0) {
		struct pipe_screen *screen = sweetgl_screen();
		struct pipe_resource templ = {
			.target = PIPE_TEXTURE_2D,
			.format = img.format,
			.width0 = (uint32_t)w,
			.height0 = (uint16_t)h,
			.depth0 = 1,
			.array_size = 1,
			.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED,
		};
		struct winsys_handle wh = {
			.type = WINSYS_HANDLE_TYPE_KMS,
			.handle = handle,
			.stride = (unsigned)pitch,
			/* client-memory surfaces start inside their first page (kernel 0157) */
			.offset = (unsigned)((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane) & vm_page_mask),
			.format = img.format,
			.modifier = DRM_FORMAT_MOD_LINEAR,
		};
		/* takes over the handle: closed with the resource, or now on failure */
		step = "mesa layout";
		img.res = screen->resource_from_handle(screen, &templ, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
		surf_res_put(&key, img.res);
	}
	if (img.res == NULL) {
		if (logged++ < 50) {
			sweet_drm_log("sweetgl: %s: surface %u %c%c%c%c %zux%zu pitch %zu plane %u: %s failed",
			    why, IOSurfaceGetID(s), FCC(fourcc), w, h, pitch, plane, step);
		}
		return false;
	}
	copy_forget(unit_held[active_unit()]);
	_mesa_EGLImageTargetTexture2DOES(target, (GLeglImageOES)&img);
	GLenum e = glGetError();
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	pipe_resource_reference(&img.res, NULL);
	if (trace_left > 0) {
		const uint8_t *base = IOSurfaceGetBaseAddress(s);
		const uint8_t *c = base ? base + (h / 2) * pitch + (w / 2) * IOSurfaceGetBytesPerElement(s) : NULL;
		GLint tex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		TRACE("%s surf %u %c%c%c%c %zux%zu pitch %zu fmt %#x/%#x -> pipe %d tex %d, center %02x%02x%02x%02x, err %#x",
		    why, IOSurfaceGetID(s), FCC(fourcc),
		    w, h, pitch, format, type, img.format, tex, c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0, e);
	}
	if (e != GL_NO_ERROR && logged++ < 50) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: EGLImageTargetTexture2D error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, e);
	}
	return e == GL_NO_ERROR;
}

/* ---- EAGLContext ---- */

static id
m_init(id self, SEL cmd, NSUInteger api, id props)
{
	struct sweetgl_ctx *ctx = api >= 2 && sweetgl_init() ? sweetgl_create((int)api, NULL) : NULL;
	struct sgl_context *c = ctx ? calloc(1, sizeof(*c)) : NULL;
	int slot = -1;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && slot < 0 && c; i++) {
		if (ctxs[i] == NULL) {
			slot = i;
			ctxs[i] = c;
		}
	}
	if (slot >= 0) {
		c->rctx = c;
		memcpy(c->disp, sgl_dispatch, sizeof(c->disp));
		c->eagl = self;
		c->api = (unsigned)api;
		c->ctx = ctx;
	}
	pthread_mutex_unlock(&ctx_lock);
	if (slot < 0) {
		free(c);
		sweetgl_destroy(ctx);
		sweet_drm_log("sweetgl: EAGLContext API %lu: no Mesa context, Apple's EAGL instead", (unsigned long)api);
		return ((id(*)(id, SEL, NSUInteger, id))orig_init)(self, cmd, api, props);
	}
	sweet_drm_log("sweetgl: EAGLContext %p API %lu on Mesa (GLI %p)", self, (unsigned long)api, c);
	return self;
}

static void
m_dealloc(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		((void (*)(id, SEL))orig_dealloc)(self, cmd);
		return;
	}
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i] == c) {
			ctxs[i] = NULL;
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	if (sgl_current == c) {
		sgl_bind(NULL);
	}
	sweetgl_destroy(c->ctx);
	free(c);
	/* EAGLContext's own init never ran: skip its dealloc */
	struct objc_super sup = {self, class_getSuperclass(objc_getClass("EAGLContext"))};
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
}

static NSUInteger
m_api(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? c->api : ((NSUInteger(*)(id, SEL))orig_api)(self, cmd);
}

static BOOL
m_set_param(id self, SEL cmd, uint32_t pname, const int32_t *value)
{
	if (lookup(self) == NULL) {
		return ((BOOL(*)(id, SEL, uint32_t, const int32_t *))orig_set_param)(self, cmd, pname, value);
	}
	sweet_drm_log("sweetgl: setParameter %#x to %d (ignored)", pname, value ? *value : 0);
	return YES;
}

static void *
m_macro(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? (void *)c : ((void *(*)(id, SEL))orig_macro)(self, cmd);
}

static BOOL
m_tex_iosurface(id self, SEL cmd, IOSurfaceRef s, NSUInteger target, NSUInteger internalformat, uint32_t width,
    uint32_t height, NSUInteger format, NSUInteger type, uint32_t plane, BOOL invert)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, IOSurfaceRef, NSUInteger, NSUInteger, uint32_t, uint32_t, NSUInteger,
		    NSUInteger, uint32_t, BOOL))orig_tex_iosurface)(self, cmd, s, target, internalformat, width,
		    height, format, type, plane, invert);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), (GLenum)format, (GLenum)type, plane, "texImageIOSurface");
}

static BOOL
m_attach(id self, SEL cmd, NSUInteger target, IOSurfaceRef s, BOOL inverted)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, NSUInteger, IOSurfaceRef, BOOL))orig_attach)(self, cmd, target, s, inverted);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), 0, 0, 0, "attachImage");
}

static __thread id current_eagl;

static BOOL
m_set_current(id cls, SEL cmd, id eagl)
{
	struct sgl_context *c = lookup(eagl);
	if (c) {
		sgl_bind(c);
		current_eagl = eagl;
		return YES;
	}
	if (sgl_current) {
		sgl_bind(NULL);
	}
	current_eagl = nil;
	return ((BOOL(*)(id, SEL, id))orig_set_current)(cls, cmd, eagl);
}

static id
m_current(id cls, SEL cmd)
{
	return current_eagl ? current_eagl : ((id(*)(id, SEL))orig_current)(cls, cmd);
}

static IMP
swap(Class cls, const char *sel, void *imp)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(sel));
	if (m == NULL) {
		sweet_drm_log("sweetgl: EAGLContext has no %s", sel);
		return NULL;
	}
	return method_setImplementation(m, (IMP)imp);
}

/*
 * Research hook (SWEETGL_TRACE): which image formats CA's GL renderer asks
 * its format table for (QuartzCore 0x18840433c in dsc-8: (ctx, format,
 * flag, &internalformat, &format, &type, &flags, &x)), with the caller.
 * The first 4 instructions become a jump here; they are replayed before
 * continuing. Needs cs_enforcement_disable (text page written).
 */
#include <mach/mach.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
void sgl_fmt_log(uint32_t fmt, uint32_t flag, uint64_t caller);
uint64_t sgl_fmt_cont;
static uint64_t sgl_cache_slide;

void
sgl_fmt_log(uint32_t fmt, uint32_t flag, uint64_t caller)
{
	static uint64_t seen[256];
	static int n;
	uint64_t key = (uint64_t)fmt << 48 ^ (uint64_t)flag << 40 ^ (caller - sgl_cache_slide);
	for (int i = 0; i < n; i++) {
		if (seen[i] == key) {
			return;
		}
	}
	if (n < 256) {
		seen[n++] = key;
		sweet_drm_log("sweetgl: CA format %u flag %u from %#llx", fmt, flag,
		    (unsigned long long)(caller - sgl_cache_slide));
	}
}

__attribute__((naked)) static void
sgl_fmt_hook(void)
{
	__asm__ volatile(
	    "stp x29, x30, [sp, #-96]!\n"
	    "mov x29, sp\n"
	    "stp x0, x1, [sp, #16]\n"
	    "stp x2, x3, [sp, #32]\n"
	    "stp x4, x5, [sp, #48]\n"
	    "stp x6, x7, [sp, #64]\n"
	    "mov x0, x1\n"
	    "mov x1, x2\n"
	    "mov x2, x30\n"
	    "bl _sgl_fmt_log\n"
	    "ldp x0, x1, [sp, #16]\n"
	    "ldp x2, x3, [sp, #32]\n"
	    "ldp x4, x5, [sp, #48]\n"
	    "ldp x6, x7, [sp, #64]\n"
	    "ldp x29, x30, [sp], #96\n"
	    "str wzr, [x5]\n"
	    "str wzr, [x4]\n"
	    "str wzr, [x3]\n"
	    "strb wzr, [x6]\n"
	    "adrp x16, _sgl_fmt_cont@PAGE\n"
	    "ldr x16, [x16, _sgl_fmt_cont@PAGEOFF]\n"
	    "br x16\n");
}

/*
 * Research entry hooks: log calls of QuartzCore renderer functions (first
 * calls in full, then counts), replaying the 4 overwritten prologue
 * instructions (position independent stack stores only).
 */
#define SGL_HOOKS 12
static const struct {
	uint64_t addr;          /* dsc-8, unslid */
	uint32_t code[4];
	const char *name;
} sgl_hook_def[SGL_HOOKS] = {
	{0x188424e54, {0xd103c3ff, 0x6d072beb, 0x6d0823e9, 0xa9096ffc}, "emit_filter"},
	{0x188427248, {0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}, "render_vibrant_color_matrix"},
	{0x188427018, {0xd102c3ff, 0x6d0533ed, 0x6d062beb, 0x6d0723e9}, "render_vibrant_filter"},
	{0x1883900d0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}, "filter_node"},
	{0x188372f74, {0xd10703ff, 0x6d1523e9, 0xa9166ffc, 0xa91767fa}, "MaskNode::apply"},
	{0x18834d888, {0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9}, "prepare_filters"},
	{0x18834f480, {0xd10303ff, 0x6d0623e9, 0xa90767fa, 0xa9085ff8}, "emit_filter_quads_color"},
	{0x1882d30b8, {0xd10243ff, 0x6d0323e9, 0xa90467fa, 0xa9055ff8}, "render_background"},
	{0x188311534, {0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}, "render_solid_background"},
	{0x1884d504c, {0xd10483ff, 0x6d0d23e9, 0xa90e6ffc, 0xa90f57f6}, "fill_round_rect"},
	{0x1882f1298, {0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4}, "fill_color_rect"},
	{0x1884af0d8, {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}, "create_shader"},
};
uint64_t sgl_hook_cont[SGL_HOOKS];
void sgl_hook_log(uint64_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t lr);

void
sgl_hook_log(uint64_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t lr)
{
	static unsigned calls[SGL_HOOKS];
	static __thread int armed;
	unsigned c = ++calls[id];
	if (id == 7 || id == 8) {
		/* x1 OGL::Layer: +0x18 Render::Layer (bg colour float4 at +0x10, bounds 4 doubles at +0x40) */
		static unsigned shown;
		uint64_t rl = *(const uint64_t *)(uintptr_t)(x1 + 0x18);
		const double *b = (const double *)(uintptr_t)(rl + 0x40);
		const float *bg = (const float *)(uintptr_t)(rl + 0x10);
		armed = 0;
		if (b[2] >= 40 && b[2] <= 160 && b[2] == b[3] && shown++ < 600) {
			uint64_t gs = *(const uint64_t *)(uintptr_t)(x0 + 0x100);
			const __fp16 *g = (const __fp16 *)(uintptr_t)(gs + 0x20);
			const __fp16 *col = (const __fp16 *)(uintptr_t)x2;
			sweet_drm_log("sweetgl: %s L %#llx %gx%g bg %g,%g,%g,%g fl %#x/%#x mode %u gcol %g,%g,%g,%g arg %g,%g,%g,%g "
			    "from %#llx", id == 7 ? "bg" : "solid", (unsigned long long)x1, b[2], b[3], bg[0], bg[1], bg[2], bg[3],
			    *(const uint32_t *)(uintptr_t)(x1 + 0x20), *(const uint16_t *)(uintptr_t)(x1 + 0x24),
			    *(const uint8_t *)(uintptr_t)(x1 + 0x26), id == 8 ? (float)g[0] : 0.f, id == 8 ? (float)g[1] : 0.f,
			    id == 8 ? (float)g[2] : 0.f, id == 8 ? (float)g[3] : 0.f, id == 8 ? (float)col[0] : 0.f,
			    id == 8 ? (float)col[1] : 0.f, id == 8 ? (float)col[2] : 0.f, id == 8 ? (float)col[3] : 0.f,
			    (unsigned long long)(lr - sgl_cache_slide));
			armed = id == 8 ? 4 : 0;
		}
		return;
	}
	if (id == 11) {
		/* x1 GLShader: byte 0 = texture function (tex_fragment_shaders index); log each variant once */
		static uint8_t seen[256][16];
		static int n;
		const uint8_t *sh = (const uint8_t *)(uintptr_t)x1;
		for (int i = 0; i < n; i++) {
			if (memcmp(seen[i], sh, 16) == 0) {
				return;
			}
		}
		if (n < 256) {
			memcpy(seen[n++], sh, 16);
		}
		const char *const *tex = (const char *const *)(uintptr_t)(0x1d5b44a10ull + sgl_cache_slide);
		char hex[3 * 0x30 + 1];
		for (int i = 0; i < 0x30; i++) {
			snprintf(hex + 3 * i, 4, "%02x ", sh[i]);
		}
		sweet_drm_log("sweetgl: create_shader fn %#x%s: %s", sh[0], sh[0] < 90 && tex[sh[0]] == NULL ? " MISSING" : "",
		    hex);
		return;
	}
	if (id == 9 || id == 10) {
		if (armed > 0) {
			armed--;
			uint64_t st = *(const uint64_t *)(uintptr_t)(x0 + 0x10);
			sweet_drm_log("sweetgl:   %s shader %u blend %u from %#llx", sgl_hook_def[id].name,
			    *(const uint8_t *)(uintptr_t)(st + 0x10), *(const uint8_t *)(uintptr_t)(st + 0x1b0),
			    (unsigned long long)(lr - sgl_cache_slide));
		}
		return;
	}
	if (id == 6) {
		/* x0 Context*, x1 Surface*: surface rect at +0x20 and the context clip shape */
		static unsigned shown;
		const int32_t *r = (const int32_t *)(uintptr_t)(x1 + 0x20);
		uint64_t clip = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(x0 + 0x100) + 8);
		if (x1 != 0 && r[2] >= 60 && r[2] <= 700 && r[3] >= 60 && r[3] <= 700 && shown++ < 300) {
			if (clip & 1 || clip == 0) {
				sweet_drm_log("sweetgl: quads_color #%u surface %d,%d %dx%d clip %#llx", c, r[0], r[1], r[2], r[3],
				    (unsigned long long)clip);
			} else {
				const int32_t *sh = (const int32_t *)(uintptr_t)clip;
				sweet_drm_log("sweetgl: quads_color #%u surface %d,%d %dx%d clip type %d [%d %d %d %d %d]", c, r[0],
				    r[1], r[2], r[3], sh[1], sh[5], sh[6], sh[7], sh[8], sh[9]);
			}
		}
	}
	if (c <= 12 || (c & (c - 1)) == 0) {
		sweet_drm_log("sweetgl: hook %s #%u from %#llx x0 %#llx x1 %#llx x2 %#llx x3 %#llx", sgl_hook_def[id].name, c,
		    (unsigned long long)(lr - sgl_cache_slide), (unsigned long long)x0, (unsigned long long)x1,
		    (unsigned long long)x2, (unsigned long long)x3);
	}
}

#define SGL_HOOK_STUB(n, w0, w1, w2, w3)                                                        \
	__attribute__((naked)) static void sgl_hook_stub##n(void)                               \
	{                                                                                       \
		__asm__ volatile(                                                               \
		    "stp x29, x30, [sp, #-224]!\n"                                              \
		    "mov x29, sp\n"                                                             \
		    "stp x0, x1, [sp, #16]\n"                                                   \
		    "stp x2, x3, [sp, #32]\n"                                                   \
		    "stp x4, x5, [sp, #48]\n"                                                   \
		    "stp x6, x7, [sp, #64]\n"                                                   \
		    "stp q0, q1, [sp, #80]\n"                                                   \
		    "stp q2, q3, [sp, #112]\n"                                                  \
		    "stp q4, q5, [sp, #144]\n"                                                  \
		    "stp q6, q7, [sp, #176]\n"                                                  \
		    "str x8, [sp, #208]\n"                                                      \
		    "mov x5, x30\n"                                                             \
		    "mov x4, x3\n"                                                              \
		    "mov x3, x2\n"                                                              \
		    "mov x2, x1\n"                                                              \
		    "mov x1, x0\n"                                                              \
		    "mov x0, #" #n "\n"                                                         \
		    "bl _sgl_hook_log\n"                                                        \
		    "ldp x0, x1, [sp, #16]\n"                                                   \
		    "ldp x2, x3, [sp, #32]\n"                                                   \
		    "ldp x4, x5, [sp, #48]\n"                                                   \
		    "ldp x6, x7, [sp, #64]\n"                                                   \
		    "ldp q0, q1, [sp, #80]\n"                                                   \
		    "ldp q2, q3, [sp, #112]\n"                                                  \
		    "ldp q4, q5, [sp, #144]\n"                                                  \
		    "ldp q6, q7, [sp, #176]\n"                                                  \
		    "ldr x8, [sp, #208]\n"                                                      \
		    "ldp x29, x30, [sp], #224\n"                                                \
		    ".long " #w0 "\n.long " #w1 "\n.long " #w2 "\n.long " #w3 "\n"              \
		    "adrp x16, _sgl_hook_cont@PAGE\n"                                           \
		    "add x16, x16, _sgl_hook_cont@PAGEOFF\n"                                    \
		    "ldr x16, [x16, #" #n " * 8]\n"                                             \
		    "br x16\n");                                                                \
	}
SGL_HOOK_STUB(0, 0xd103c3ff, 0x6d072beb, 0x6d0823e9, 0xa9096ffc)
SGL_HOOK_STUB(1, 0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8)
SGL_HOOK_STUB(2, 0xd102c3ff, 0x6d0533ed, 0x6d062beb, 0x6d0723e9)
SGL_HOOK_STUB(3, 0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd)
SGL_HOOK_STUB(4, 0xd10703ff, 0x6d1523e9, 0xa9166ffc, 0xa91767fa)
SGL_HOOK_STUB(5, 0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9)
SGL_HOOK_STUB(6, 0xd10303ff, 0x6d0623e9, 0xa90767fa, 0xa9085ff8)
SGL_HOOK_STUB(7, 0xd10243ff, 0x6d0323e9, 0xa90467fa, 0xa9055ff8)
SGL_HOOK_STUB(8, 0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8)
SGL_HOOK_STUB(9, 0xd10483ff, 0x6d0d23e9, 0xa90e6ffc, 0xa90f57f6)
SGL_HOOK_STUB(10, 0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4)
SGL_HOOK_STUB(11, 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6)
static void (*const sgl_hook_stub[SGL_HOOKS])(void) = {sgl_hook_stub0, sgl_hook_stub1, sgl_hook_stub2, sgl_hook_stub3,
    sgl_hook_stub4, sgl_hook_stub5, sgl_hook_stub6, sgl_hook_stub7, sgl_hook_stub8, sgl_hook_stub9, sgl_hook_stub10,
    sgl_hook_stub11};

/* 16 bytes at t become ldr x16, #8; br x16; .quad to (after checking they are want) */
static bool
sgl_patch(uint32_t *t, const uint32_t want[4], void (*to)(void))
{
	if (memcmp(t, want, 16) != 0) {
		sweet_drm_log("sweetgl: patch: unexpected code at %p (%08x)", (void *)t, t[0]);
		return false;
	}
	vm_address_t page = (vm_address_t)t & ~(vm_address_t)vm_page_mask;
	if (vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		return false;
	}
	uint32_t patch[4] = {0x58000050, 0xd61f0200, 0, 0};
	uint64_t target = (uint64_t)(uintptr_t)to;
	memcpy(&patch[2], &target, 8);
	memcpy(t, patch, sizeof(patch));
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(t, sizeof(patch));
	return true;
}

static void
sgl_install_hooks(void)
{
	for (int i = 0; i < SGL_HOOKS; i++) {
		uint32_t *t = (uint32_t *)(uintptr_t)(sgl_hook_def[i].addr + sgl_cache_slide);
		sgl_hook_cont[i] = (uint64_t)(uintptr_t)t + 16;
		bool ok = sgl_patch(t, sgl_hook_def[i].code, sgl_hook_stub[i]);
		sweet_drm_log("sweetgl: hook %s at %p: %s", sgl_hook_def[i].name, (void *)t, ok ? "installed" : "FAILED");
	}
}

static void
sgl_install_fmt_hook(void)
{
	size_t len = 0;
	const uint8_t *cache = _dyld_get_shared_cache_range(&len);
	if (cache == NULL) {
		return;
	}
	sgl_cache_slide = (uint64_t)(uintptr_t)cache - 0x180000000ull;
	uint32_t *t = (uint32_t *)(uintptr_t)(0x18840433cull + sgl_cache_slide);
	static const uint32_t want[4] = {0xb90000bf, 0xb900009f, 0xb900007f, 0x390000df};
	if (memcmp(t, want, sizeof(want)) != 0) {
		sweet_drm_log("sweetgl: format hook: unexpected code at %p (%08x), not hooked", (void *)t, t[0]);
		return;
	}
	sgl_fmt_cont = (uint64_t)(uintptr_t)t + 16;
	vm_address_t page = (vm_address_t)t & ~(vm_address_t)vm_page_mask;
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("sweetgl: format hook: vm_protect rw %d", kr);
		return;
	}
	uint32_t patch[4] = {0x58000050, 0xd61f0200, 0, 0};    /* ldr x16, #8; br x16; .quad hook */
	uint64_t target = (uint64_t)(uintptr_t)sgl_fmt_hook;
	memcpy(&patch[2], &target, 8);
	memcpy(t, patch, sizeof(patch));
	kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(t, sizeof(patch));
	sweet_drm_log("sweetgl: format hook installed at %p (slide %#llx), protect back %d", (void *)t,
	    (unsigned long long)sgl_cache_slide, kr);
	sgl_install_hooks();
}

/*
 * QuartzCore (dsc-8) GL renderer fixes, see vcm_func and nop_func:
 * tex_fragment_shaders (0x1d5b44a10) entries, and create_shader's
 * per-function uniform switch (byte table 0x18855a1e4, index function -
 * 0x11) set to look up the uniform names the function uses instead of
 * nothing (case 0xde).
 */
static const struct {
	uint8_t fn;
	const char *expr;
	uint8_t uniforms;       /* uniform switch case: 0 color_matrix, 0x95 minmax_dir/bgcolor/map/args, 0xde none */
} sgl_texfns[] = {{0x2d, "sgl_vcm(s0)", 0}, {0x14, "sgl_cm4(s0)", 0}, {0x26, "sgl_nop()", 0xde},
    {0x27, "sgl_vblur(s1)", 0x95}};

static bool
sgl_write_const(void *at, const void *val, size_t len, vm_prot_t back)
{
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)vm_page_mask;
	if (vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		return false;
	}
	memcpy(at, val, len);
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, back);
	return true;
}

static void
sgl_install_texfns(void)
{
	size_t len = 0;
	const uint8_t *cache = _dyld_get_shared_cache_range(&len);
	if (cache == NULL) {
		return;
	}
	uint64_t slide = (uint64_t)(uintptr_t)cache - 0x180000000ull;
	const char **table = (const char **)(uintptr_t)(0x1d5b44a10ull + slide);
	uint8_t *cases = (uint8_t *)(uintptr_t)(0x18855a1e4ull - 0x11 + slide);
	if (table[0x13] == NULL || strcmp(table[0x13], "colorMatrix4x4Bias(s0)") != 0 || cases[0x13] != 0) {
		sweet_drm_log("sweetgl: texture function fixes: unexpected QuartzCore, not installed");
		return;
	}
	for (size_t i = 0; i < sizeof(sgl_texfns) / sizeof(sgl_texfns[0]); i++) {
		uint8_t fn = sgl_texfns[i].fn, uniforms = sgl_texfns[i].uniforms;
		if (table[fn] != NULL || cases[fn] != 0xde) {
			sweet_drm_log("sweetgl: texture function %#x: unexpected (%p %#x), not installed", fn,
			    (void *)table[fn], cases[fn]);
			continue;
		}
		bool ok = sgl_write_const(&table[fn], &sgl_texfns[i].expr, sizeof(char *), VM_PROT_READ) &&
		    (uniforms == 0xde || sgl_write_const(&cases[fn], &uniforms, 1, VM_PROT_READ | VM_PROT_EXECUTE));
		sweet_drm_log("sweetgl: texture function %#x = %s: %s", fn, sgl_texfns[i].expr, ok ? "installed" : "FAILED");
	}
}

/* Called at load. CA picks its GLES renderer by itself once initWithAPI:3 succeeds. */
void
sweetgl_eagl_install(void)
{
	const char *e = getenv("SWEETGL_GLERR");
	sgl_check_errors = e && e[0] == '1';
	const char *t = getenv("SWEETGL_TRACE");
	trace_left = t && t[0] == '1' ? 20000 : 0;
	dump_shaders = trace_left > 0;
	sgl_install_texfns();
	if (trace_left > 0) {
		sgl_install_fmt_hook();
	}
	Class cls = objc_getClass("EAGLContext");
	if (cls == Nil) {
		sweet_drm_log("sweetgl: no EAGLContext class");
		return;
	}
	Class meta = object_getClass((id)cls);
	orig_init = swap(cls, "initWithAPI:properties:", m_init);
	orig_dealloc = swap(cls, "dealloc", m_dealloc);
	orig_api = swap(cls, "API", m_api);
	orig_set_param = swap(cls, "setParameter:to:", m_set_param);
	orig_macro = swap(cls, "getMacroContextPrivate", m_macro);
	orig_tex_iosurface = swap(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
	    m_tex_iosurface);
	orig_attach = swap(cls, "attachImage:toCoreSurface:invertedRender:", m_attach);
	orig_set_current = swap(meta, "setCurrentContext:", m_set_current);
	orig_current = swap(meta, "currentContext", m_current);
	sweet_drm_log("sweetgl: EAGL shim installed%s", sgl_check_errors ? " (GL error checks on)" : "");
}
