/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_GL_EFFECTS_H
#define LABWC_GL_EFFECTS_H

#include <stdbool.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/box.h>
#include <wayland-server-core.h>

/**
 * Initialize GL shader pipelines and state.
 * Returns true on success, false if GLES2 is not available.
 */
bool gl_effects_init(struct wlr_renderer *renderer);

/**
 * Clean up GL resources and shaders.
 */
void gl_effects_finish(void);

/**
 * Checks whether GL effects (shaders) are supported and ready.
 */
bool gl_effects_is_available(void);

/**
 * Render a texture into destination with anti-aliased rounded corners using SDF shader.
 */
bool gl_effects_render_texture_rounded(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	struct wlr_texture *texture,
	const struct wlr_fbox *src_box,
	const struct wlr_box *dst_box,
	float corner_radius,
	float alpha,
	enum wl_output_transform transform);

/**
 * Anti-aliased corner clearing for window boxes on the output buffer.
 */
bool gl_effects_clip_view_corners(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	const struct wlr_box *box,
	float corner_radius);

/**
 * Capture background region behind window before window contents are composited.
 */
bool gl_effects_capture_background(
	struct wlr_renderer *renderer,
	struct wlr_buffer *target_buffer,
	const struct wlr_box *box);

/**
 * Perform Dual-Kawase blur and smooth rounded corner masking on the window box.
 */
bool gl_effects_apply_dual_kawase_blur(
	struct wlr_renderer *renderer,
	struct wlr_buffer *target_buffer,
	const struct wlr_box *box,
	float corner_radius,
	int passes,
	float blur_radius,
	bool blur_enabled);

struct view;

/**
 * Render a view's client surfaces with anti-aliased rounded corners using SDF shader.
 */
bool gl_effects_render_view_content(
	struct wlr_renderer *renderer,
	struct wlr_buffer *dst_buffer,
	struct view *view,
	int offset_x,
	int offset_y,
	float corner_radius,
	float alpha);

#endif /* LABWC_GL_EFFECTS_H */

