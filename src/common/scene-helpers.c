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


#define MAX_NODES 4096

struct node_state {
	struct wlr_scene_node *node;
	float opacity;
	float color[4];
};

static struct node_state saved_states[MAX_NODES];
static int saved_count = 0;

static void
hide_recursive(struct wlr_scene_node *node)
{
	if (!node || !node->enabled) {
		return;
	}

	if (saved_count < MAX_NODES) {
		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
			saved_states[saved_count].node = node;
			saved_states[saved_count].opacity = buffer->opacity;
			buffer->opacity = 0.0f;
			saved_count++;
		} else if (node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
			saved_states[saved_count].node = node;
			memcpy(saved_states[saved_count].color, rect->color, sizeof(float) * 4);
			rect->color[3] = 0.0f;
			saved_count++;
		}
	} else {
		wlr_log(WLR_ERROR, "hide_recursive: saved_states overflow (%d)", saved_count);
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			hide_recursive(child);
		}
	}
}

static void
restore_nodes(void)
{
	for (int i = saved_count - 1; i >= 0; i--) {
		struct wlr_scene_node *n = saved_states[i].node;
		if (n->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(n);
			buffer->opacity = saved_states[i].opacity;
		} else if (n->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(n);
			memcpy(rect->color, saved_states[i].color, sizeof(float) * 4);
		}
	}
	saved_count = 0;
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

	if (has_effects) {
		/* Temporarily make all window nodes transparent so wlroots scene does not draw rectangular buffers,
		 * while still allowing wlroots to accurately track damage and send frame_done events. */
		struct view *view;
		for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (view->mapped && !view->shaded && view->maximized == VIEW_AXIS_NONE && !view->fullscreen && view->content_tree) {
				hide_recursive(&view->scene_tree->node);
			}
		}
	}

	if (!wlr_scene_output_build_state(scene_output, state, NULL)) {
		wlr_log(WLR_ERROR, "Failed to build output state for %s",
			wlr_output->name);
		return false;
	}

	if (has_effects) {
		/* Restore original visibility */
		restore_nodes();
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
			struct border margin = ssd_thickness(view);
			float outer_r = rc.corner_radius * wlr_output->scale;
			float inner_r = MAX(rc.corner_radius - rc.theme->border_width, 0.0f) * wlr_output->scale;
			struct wlr_box content_box = {
				.x = (int)roundf((view->current.x - scene_output->x) * wlr_output->scale),
				.y = (int)roundf((view->current.y - scene_output->y) * wlr_output->scale),
				.width = (int)roundf(view->current.width * wlr_output->scale),
				.height = (int)roundf(view->current.height * wlr_output->scale),
			};
			struct wlr_box window_box = {
				.x = (int)roundf((view->current.x - margin.left - scene_output->x) * wlr_output->scale),
				.y = (int)roundf((view->current.y - margin.top - scene_output->y) * wlr_output->scale),
				.width = (int)roundf((view->current.width + margin.left + margin.right) * wlr_output->scale),
				.height = (int)roundf((view->current.height + margin.top + margin.bottom) * wlr_output->scale),
			};


			if (rc.blur_enabled) {
				if (!gl_effects_apply_dual_kawase_blur(
					server.renderer,
					state->buffer,
					&content_box,
					inner_r,
					rc.blur_passes,
					rc.blur_radius,
					rc.blur_enabled)) {
					wlr_log(WLR_ERROR, "gl_effects_apply_dual_kawase_blur failed for view %p", view);
				}
			}

			if (rc.corner_radius > 0) {
				if (!gl_effects_render_view_content(
					server.renderer,
					state->buffer,
					view,
					&window_box,
					scene_output->x,
					scene_output->y,
					wlr_output->scale,
					outer_r,
					1.0f)) {
					wlr_log(WLR_ERROR, "gl_effects_render_view_content failed for view %p", view);
				}
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
