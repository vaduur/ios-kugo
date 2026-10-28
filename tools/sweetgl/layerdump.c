/*
 * SpringBoard side of sweetgl (sweetenv SpringBoard:DYLD_INSERT_LIBRARIES=...).
 *
 * Research aid (SWEETGL_LAYERDUMP=1): every
 * 30 s from 60 s on, the layers of all windows that carry filters, a
 * compositing filter or a material/backdrop class, with their view, frame,
 * colour, opacity and visibility. To see what CA is asked to draw for the
 * CC toggle circles and App Library plates that its GL renderer drops.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

#include "sweet_drm.h"

typedef struct { double x, y, w, h; } rect_t;

static SEL S(const char *n) { return sel_registerName(n); }
static id call(id o, const char *s) { return o ? ((id (*)(id, SEL))objc_msgSend)(o, S(s)) : nil; }
static unsigned long count(id a) { return a ? ((unsigned long (*)(id, SEL))objc_msgSend)(a, S("count")) : 0; }
static id at(id a, unsigned long i) { return ((id (*)(id, SEL, unsigned long))objc_msgSend)(a, S("objectAtIndex:"), i); }

static const char *
desc(id o, char *buf, size_t len)
{
	buf[0] = 0;
	if (o == nil) {
		return "-";
	}
	CFStringRef d = CFCopyDescription((CFTypeRef)o);
	if (d != NULL) {
		CFStringGetCString(d, buf, (CFIndex)len, kCFStringEncodingUTF8);
		CFRelease(d);
	}
	for (char *c = buf; *c; c++) {
		if (*c == '\n') {
			*c = ' ';
		}
	}
	return buf;
}

static int lines;

static void
walk(id layer, int depth, bool deep)
{
	if (layer == nil || lines > 900) {
		return;
	}
	id cf = call(layer, "compositingFilter"), filters = call(layer, "filters");
	const char *cls = object_getClassName(layer);
	id delegate = call(layer, "delegate");
	const char *view = delegate ? object_getClassName(delegate) : "-";
	deep = deep || strstr(view, "PodBackground") != NULL;
	bool interesting = deep || cf != nil || count(filters) > 0 || strstr(cls, "Backdrop") || strstr(cls, "Material") ||
	    strstr(view, "Material") || strstr(view, "RoundButton") || strstr(view, "Pod") || strstr(view, "Visual");
	if (interesting) {
		rect_t b = ((rect_t (*)(id, SEL))objc_msgSend)(layer, S("bounds"));
		rect_t r = ((rect_t (*)(id, SEL, rect_t, id))objc_msgSend)(layer, S("convertRect:toLayer:"), b, nil);
		float op = ((float (*)(id, SEL))objc_msgSend)(layer, S("opacity"));
		BOOL hid = ((BOOL (*)(id, SEL))objc_msgSend)(layer, S("isHidden"));
		double rad = ((double (*)(id, SEL))objc_msgSend)(layer, S("cornerRadius"));
		const void *bg = ((const void *(*)(id, SEL))objc_msgSend)(layer, S("backgroundColor"));
		char b1[200], b2[300], b3[160];
		sweet_drm_log("sweetgl: L%d %s (%s) %.0f,%.0f %.0fx%.0f op %.2f hid %d rad %.1f mask %d contents %d bg %.150s cf %.150s",
		    depth, cls, view, r.x, r.y, r.w, r.h, op, hid, rad, call(layer, "mask") != nil,
		    call(layer, "contents") != nil, desc((id)bg, b3, sizeof(b3)), desc(cf, b1, sizeof(b1)));
		for (unsigned long i = 0; i < count(filters) && i < 4; i++) {
			id f = at(filters, i);
			sweet_drm_log("sweetgl:   filter %lu: %.300s", i, desc(f, b2, sizeof(b2)));
			if (deep) {
				static const char *keys[] = {"inputColorMatrix", "inputAmount", "inputColor", "inputRadius", "enabled"};
				for (int k = 0; k < 5; k++) {
					id key = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
					    S("stringWithUTF8String:"), keys[k]);
					id v = ((id (*)(id, SEL, id))objc_msgSend)(f, S("valueForKey:"), key);
					if (v != nil) {
						sweet_drm_log("sweetgl:     %s = %.300s", keys[k], desc(v, b2, sizeof(b2)));
					}
				}
			}
		}
		if (deep) {
			sweet_drm_log("sweetgl:     contentsFormat %.60s cornerCurve %.40s masksToBounds %d compositingFilter %.100s",
			    desc(call(layer, "contentsFormat"), b1, sizeof(b1)), desc(call(layer, "cornerCurve"), b2, sizeof(b2)),
			    ((BOOL (*)(id, SEL))objc_msgSend)(layer, S("masksToBounds")), desc(cf, b3, sizeof(b3)));
		}
		lines++;
	}
	bool sub_deep = deep || strstr(view, "RoundButton") || strstr(view, "PodBackground");
	id subs = call(layer, "sublayers");
	for (unsigned long i = 0; i < count(subs); i++) {
		walk(at(subs, i), depth + 1, sub_deep);
	}
}

static void
dump(void *ctx)
{
	static int round;
	Class w = objc_getClass("UIWindow");
	id wins = w ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)w, S("allWindowsIncludingInternalWindows:onlyVisibleWindows:"),
	    YES, YES) : nil;
	lines = 0;
	sweet_drm_log("sweetgl: layer dump %d: %lu windows", round, count(wins));
	for (unsigned long i = 0; i < count(wins); i++) {
		id win = at(wins, i);
		sweet_drm_log("sweetgl: window %s", object_getClassName(win));
		walk(call(win, "layer"), 0, false);
	}
	if (++round < 8) {
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, dump);
	}
	(void)ctx;
}

void
sweetgl_layerdump_start(void)
{
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, dump);
}

/*
 * CA's GL renderer never draws a solid-colour layer whose only filter is
 * vibrantColorMatrix (CC toggle off-circles, App Library category plates:
 * it builds no source surface for it, so the filtered result is empty).
 * In SpringBoard such layers lose the filter and get their background
 * colour's alpha scaled instead: the lighter translucent shape the filter
 * makes of the backdrop.
 * ponytail: brightness from the matrix bias only, no saturation; plain
 * CALayer class only (material/backdrop layers render fine).
 */
#include <CoreGraphics/CoreGraphics.h>

static IMP orig_set_filters, orig_set_bg;
static char vibrant_key;

static float
vibrant_alpha(id filter)
{
	id key = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), S("stringWithUTF8String:"),
	    "inputColorMatrix");
	id v = ((id (*)(id, SEL, id))objc_msgSend)(filter, S("valueForKey:"), key);
	float m[20] = {0};
	/* an NSValue holding a CAColorMatrix (20 floats) */
	if (v != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(v, S("respondsToSelector:"), S("getValue:size:"))) {
		((void (*)(id, SEL, void *, unsigned long))objc_msgSend)(v, S("getValue:size:"), m, sizeof(m));
	}
	float bias = (m[4] + m[9] + m[14]) / 3;     /* rows r,g,b: 5th column */
	float a = bias > 0.05f ? bias : 0.15f;
	a = a > 0.5f ? 0.5f : a;
	static int logged;
	if (logged++ < 20) {
		sweet_drm_log("sweetgl: vibrantColorMatrix -> fill alpha %.2f (bias %.3f, m11 %.3f m22 %.3f m33 %.3f)", a, bias, m[0],
		    m[6], m[12]);
	}
	return a;
}

static id
scaled_color(id layer, CGColorRef c)
{
	id f = objc_getAssociatedObject(layer, &vibrant_key);
	if (c == NULL || f == nil) {
		return (id)c;
	}
	float a = ((float (*)(id, SEL))objc_msgSend)(f, S("floatValue"));
	CGColorRef s = CGColorCreateCopyWithAlpha(c, CGColorGetAlpha(c) * a);
	return (id)CFAutorelease(s);
}

static void
set_bg(id self, SEL cmd, CGColorRef c)
{
	((void (*)(id, SEL, id))orig_set_bg)(self, cmd, scaled_color(self, c));
}

static void
set_filters(id self, SEL cmd, id filters)
{
	id one = count(filters) == 1 ? at(filters, 0) : nil;
	id type = one ? call(one, "type") : nil;
	const char *t = type ? ((const char *(*)(id, SEL))objc_msgSend)(type, S("UTF8String")) : "";
	if (object_getClass(self) == objc_getClass("CALayer") && strcmp(t, "vibrantColorMatrix") == 0 &&
	    call(self, "contents") == nil) {
		id num = ((id (*)(id, SEL, float))objc_msgSend)((id)objc_getClass("NSNumber"), S("numberWithFloat:"),
		    vibrant_alpha(one));
		CGColorRef bg = (CGColorRef)(void *)call(self, "backgroundColor");
		bool had = objc_getAssociatedObject(self, &vibrant_key) != nil;
		objc_setAssociatedObject(self, &vibrant_key, num, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		((void (*)(id, SEL, id))orig_set_filters)(self, cmd, nil);
		if (!had) {
			set_bg(self, S("setBackgroundColor:"), bg);
		}
		return;
	}
	objc_setAssociatedObject(self, &vibrant_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	((void (*)(id, SEL, id))orig_set_filters)(self, cmd, filters);
}

void
sweetgl_vibrant_fill_install(void)
{
	Class c = objc_getClass("CALayer");
	Method mf = class_getInstanceMethod(c, S("setFilters:")), mb = class_getInstanceMethod(c, S("setBackgroundColor:"));
	if (mf == NULL || mb == NULL) {
		return;
	}
	orig_set_filters = method_setImplementation(mf, (IMP)set_filters);
	orig_set_bg = method_setImplementation(mb, (IMP)set_bg);
	sweet_drm_log("sweetgl: vibrant fill workaround installed");
}
