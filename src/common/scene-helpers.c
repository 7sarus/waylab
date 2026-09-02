// SPDX-License-Identifier: GPL-2.0-only

#include "common/scene-helpers.h"
#include <assert.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "config/rcxml.h"
#include "gl-effects.h"
#include "labwc.h"
#include "magnifier.h"
#include "output.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

struct wlr_surface *
lab_wlr_surface_from_node(struct wlr_scene_node *node)
{
	struct wlr_scene_buffer *buffer;
	struct wlr_scene_surface *scene_surface;

	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		buffer = wlr_scene_buffer_from_node(node);
		scene_surface = wlr_scene_surface_try_from_buffer(buffer);
		if (scene_surface) {
			return scene_surface->surface;
		}
	}
	return NULL;
}

struct wlr_scene_tree *
lab_wlr_scene_tree_create(struct wlr_scene_tree *parent)
{
	struct wlr_scene_tree *tree = wlr_scene_tree_create(parent);
	die_if_null(tree);
	return tree;
}

struct wlr_scene_rect *
lab_wlr_scene_rect_create(struct wlr_scene_tree *parent,
		int width, int height, const float color[static 4])
{
	struct wlr_scene_rect *rect =
		wlr_scene_rect_create(parent, width, height, color);
	die_if_null(rect);
	return rect;
}

struct wlr_scene_buffer *
lab_wlr_scene_buffer_create(struct wlr_scene_tree *parent,
		struct wlr_buffer *buffer)
{
	struct wlr_scene_buffer *scene_buffer =
		wlr_scene_buffer_create(parent, buffer);
	die_if_null(scene_buffer);
	return scene_buffer;
}

struct wlr_scene_node *
lab_wlr_scene_get_prev_node(struct wlr_scene_node *node)
{
	assert(node);
	struct wlr_scene_node *prev;
	prev = wl_container_of(node->link.prev, node, link);
	if (&prev->link == &node->parent->children) {
		return NULL;
	}
	return prev;
}

/*
 * This is a slightly modified copy of scene_output_damage(),
 * required to properly add the magnifier damage to scene_output
 * ->damage_ring and scene_output->pending_commit_damage.
 *
 * The only difference is code style and removal of wlr_output_schedule_frame().
 */
static void
scene_output_damage(struct wlr_scene_output *scene_output,
		const pixman_region32_t *damage)
{
	struct wlr_output *output = scene_output->output;

	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, 0, 0, output->width, output->height);

	if (pixman_region32_not_empty(&clipped)) {
		wlr_damage_ring_add(&scene_output->damage_ring, &clipped);
		pixman_region32_union(&scene_output->WLR_PRIVATE.pending_commit_damage,
			&scene_output->WLR_PRIVATE.pending_commit_damage, &clipped);
	}

	pixman_region32_fini(&clipped);
}

struct opacity_save_state {
	struct wlr_scene_buffer *buffers[1024];
	float opacities[1024];
	int count;
};

static void save_and_clear_opacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct opacity_save_state *state = data;
	if (state->count < 1024) {
		state->buffers[state->count] = buffer;
		state->opacities[state->count] = buffer->opacity;
		state->count++;
	}
	buffer->opacity = 0.0f;
}

/*
 * This is a copy of wlr_scene_output_commit()
 * as it doesn't use the pending state at all.
 */
bool
lab_wlr_scene_output_commit(struct wlr_scene_output *scene_output,
		struct wlr_output_state *state)
{
	assert(scene_output);
	assert(state);
	struct wlr_output *wlr_output = scene_output->output;
	struct output *output = wlr_output->data;
	bool wants_magnification = output_wants_magnification(output);

	/*
	 * FIXME: Regardless of wants_magnification, we are currently adding
	 * damages to next frame when magnifier is shown, which forces
	 * rendering on every output commit and overloads CPU.
	 * We also need to verify the necessity of wants_magnification.
	 */
	if (!wlr_scene_output_needs_frame(scene_output) && !wants_magnification) {
		return true;
	}

	bool has_effects = (gl_effects_is_available() && (rc.blur_enabled || rc.corner_radius > 0));

	struct opacity_save_state op_state = { .count = 0 };

	if (has_effects) {
		/* Force full swapchain buffer damage so no stale window pixels from past double/triple buffers are captured */
		wlr_damage_ring_add_whole(&scene_output->damage_ring);

		/* Temporarily make content_tree nodes transparent so wlroots scene does not draw rectangular buffers,
		 * while still collecting damage and sending frame_done events. */
		struct view *view;
		for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (view->mapped && !view->shaded && view->maximized == VIEW_AXIS_NONE && !view->fullscreen && view->content_tree) {
				wlr_scene_node_for_each_buffer(&view->content_tree->node, save_and_clear_opacity, &op_state);
			}
		}
	}

	if (!wlr_scene_output_build_state(scene_output, state, NULL)) {
		wlr_log(WLR_ERROR, "Failed to build output state for %s",
			wlr_output->name);
		return false;
	}

	if (has_effects) {
		/* Restore original opacities */
		for (int i = 0; i < op_state.count; i++) {
			op_state.buffers[i]->opacity = op_state.opacities[i];
		}
	}

	if (state->tearing_page_flip) {
		if (!wlr_output_test_state(wlr_output, state)) {
			state->tearing_page_flip = false;
		}
	}

	struct wlr_box additional_damage = {0};
	if (state->buffer && gl_effects_is_available() && (rc.blur_enabled || rc.corner_radius > 0)) {
		struct view *view;
		/* Iterate back to front (bottom of stack to top) for correct z-order composition */
		for_each_view_reverse(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (!view->mapped || view->shaded || view->maximized != VIEW_AXIS_NONE || view->fullscreen || !view->content_tree) {
				continue;
			}
			float inner_r = MAX(rc.corner_radius - rc.theme->border_width, 0.0f) * wlr_output->scale;
			struct wlr_box content_box = {
				.x = (view->current.x - scene_output->x) * wlr_output->scale,
				.y = (view->current.y - scene_output->y) * wlr_output->scale,
				.width = view->current.width * wlr_output->scale,
				.height = view->current.height * wlr_output->scale,
			};

			if (rc.blur_enabled) {
				gl_effects_apply_dual_kawase_blur(
					server.renderer,
					state->buffer,
					&content_box,
					inner_r,
					rc.blur_passes,
					rc.blur_radius,
					rc.blur_enabled);
			}

			if (rc.corner_radius > 0) {
				gl_effects_render_view_content(
					server.renderer,
					state->buffer,
					view,
					&content_box,
					scene_output->x,
					scene_output->y,
					wlr_output->scale,
					inner_r,
					1.0f);
			}
		}
	}
	if (state->buffer && magnifier_is_enabled()) {
		magnifier_draw(output, state->buffer, &additional_damage);
	}

	bool committed = wlr_output_commit_state(wlr_output, state);
	/*
	 * Handle case where the output state test for tearing succeeded,
	 * but actual commit failed. Retry without tearing.
	 */
	if (!committed && state->tearing_page_flip) {
		state->tearing_page_flip = false;
		committed = wlr_output_commit_state(wlr_output, state);
	}
	if (committed) {
		if (state == &output->pending) {
			wlr_output_state_finish(&output->pending);
			wlr_output_state_init(&output->pending);
		}
	} else {
		wlr_log(WLR_INFO, "Failed to commit output %s",
			wlr_output->name);
		return false;
	}

	if (!wlr_box_empty(&additional_damage)) {
		pixman_region32_t region;
		pixman_region32_init_rect(&region,
			additional_damage.x, additional_damage.y,
			additional_damage.width, additional_damage.height);
		scene_output_damage(scene_output, &region);
		pixman_region32_fini(&region);
	}

	return true;
}
