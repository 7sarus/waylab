// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/macros.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

void
ssd_border_create(struct ssd *ssd)
{
	assert(ssd);
	assert(!ssd->border.tree);

	struct view *view = ssd->view;
	struct theme *theme = rc.theme;
	int width = view->current.width;
	int height = view_effective_height(view, /* use_pending */ false);
	int full_width = width + 2 * theme->border_width;
	int corner_width = ssd_get_corner_width();

	ssd->border.tree = lab_wlr_scene_tree_create(ssd->tree);
	wlr_scene_node_set_position(&ssd->border.tree->node, -theme->border_width, 0);

	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[active];
		subtree->tree = lab_wlr_scene_tree_create(ssd->border.tree);
		struct wlr_scene_tree *parent = subtree->tree;
		wlr_scene_node_set_enabled(&parent->node, active);
		float *color = theme->window[active].border_color;

		subtree->left = lab_wlr_scene_rect_create(parent,
			theme->border_width, height, color);
		wlr_scene_node_set_position(&subtree->left->node, 0, 0);

		subtree->right = lab_wlr_scene_rect_create(parent,
			theme->border_width, height, color);
		wlr_scene_node_set_position(&subtree->right->node,
			theme->border_width + width, 0);

		subtree->bottom = lab_wlr_scene_rect_create(parent,
			full_width, theme->border_width, color);
		wlr_scene_node_set_position(&subtree->bottom->node,
			0, height);

		subtree->top = lab_wlr_scene_rect_create(parent,
			MAX(width - 2 * corner_width, 0), theme->border_width, color);
		wlr_scene_node_set_position(&subtree->top->node,
			theme->border_width + corner_width,
			-(ssd->titlebar.height + theme->border_width));

		struct wlr_buffer *c_bl = &theme->window[active].corner_bottom_left_normal->base;
		struct wlr_buffer *c_br = &theme->window[active].corner_bottom_right_normal->base;
		subtree->corner_bottom_left = lab_wlr_scene_buffer_create(parent, c_bl);
		subtree->corner_bottom_right = lab_wlr_scene_buffer_create(parent, c_br);
	}

	if (view->maximized == VIEW_AXIS_BOTH) {
		wlr_scene_node_set_enabled(&ssd->border.tree->node, false);
	}

	if (view->current.width > 0 && view->current.height > 0) {
		/*
		 * The SSD is recreated by a Reconfigure request
		 * thus we may need to handle squared corners.
		 */
		ssd_border_update(ssd);
	}
}

void
ssd_border_update(struct ssd *ssd)
{
	assert(ssd);
	assert(ssd->border.tree);

	struct view *view = ssd->view;
	if (view->maximized == VIEW_AXIS_BOTH
			&& ssd->border.tree->node.enabled) {
		/* Disable borders on maximize */
		wlr_scene_node_set_enabled(&ssd->border.tree->node, false);
		ssd->margin = ssd_thickness(ssd->view);
	}

	if (view->maximized == VIEW_AXIS_BOTH) {
		return;
	} else if (!ssd->border.tree->node.enabled) {
		/* And re-enabled them when unmaximized */
		wlr_scene_node_set_enabled(&ssd->border.tree->node, true);
		ssd->margin = ssd_thickness(ssd->view);
	}

	struct theme *theme = rc.theme;

	int width = view->current.width;
	int height = view_effective_height(view, /* use_pending */ false);
	int full_width = width + 2 * theme->border_width;
	int corner_width = ssd_get_corner_width();

	/*
	 * From here on we have to cover the following border scenarios:
	 * Non-tiled (partial border, rounded corners):
	 *    _____________
	 *   o           oox
	 *  |---------------|
	 *  |_______________|
	 *
	 * Tiled (full border, squared corners):
	 *   _______________
	 *  |o           oox|
	 *  |---------------|
	 *  |_______________|
	 *
	 * Tiled or non-tiled with zero title height (full boarder, no title):
	 *   _______________
	 *  |_______________|
	 */

	bool rounded = !ssd->state.was_squared && rc.corner_radius > 0;
	int side_height;
	int side_y;
	if (ssd->state.was_squared) {
		side_height = height + ssd->titlebar.height;
		side_y = -ssd->titlebar.height;
	} else if (rounded) {
		side_height = MAX(height - corner_width, 0);
		side_y = 0;
	} else {
		side_height = height;
		side_y = 0;
	}

	int top_width = ssd->titlebar.height <= 0 || ssd->state.was_squared
		? full_width
		: MAX(width - 2 * corner_width, 0);
	int top_x = ssd->titlebar.height <= 0 || ssd->state.was_squared
		? 0
		: theme->border_width + corner_width;

	int bottom_width = rounded
		? MAX(width - 2 * corner_width, 0)
		: full_width;
	int bottom_x = rounded
		? theme->border_width + corner_width
		: 0;

	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[active];

		wlr_scene_rect_set_size(subtree->left,
			theme->border_width, side_height);
		wlr_scene_node_set_position(&subtree->left->node,
			0, side_y);

		wlr_scene_rect_set_size(subtree->right,
			theme->border_width, side_height);
		wlr_scene_node_set_position(&subtree->right->node,
			theme->border_width + width, side_y);

		wlr_scene_rect_set_size(subtree->bottom,
			bottom_width, theme->border_width);
		wlr_scene_node_set_position(&subtree->bottom->node,
			bottom_x, height);

		wlr_scene_rect_set_size(subtree->top,
			top_width, theme->border_width);
		wlr_scene_node_set_position(&subtree->top->node,
			top_x, -(ssd->titlebar.height + theme->border_width));

		wlr_scene_node_set_enabled(&subtree->corner_bottom_left->node, rounded);
		wlr_scene_node_set_enabled(&subtree->corner_bottom_right->node, rounded);

		if (rounded) {
			wlr_scene_node_set_position(&subtree->corner_bottom_left->node,
				0, height - corner_width);
			wlr_scene_node_set_position(&subtree->corner_bottom_right->node,
				width - corner_width + theme->border_width, height - corner_width);
		}
	}
}

void
ssd_border_destroy(struct ssd *ssd)
{
	assert(ssd);
	assert(ssd->border.tree);

	wlr_scene_node_destroy(&ssd->border.tree->node);
	ssd->border = (struct ssd_border_scene){0};
}
