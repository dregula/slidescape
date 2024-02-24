/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2023  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "viewer.h"
#include "annotation.h"
#include "platform.h"
#include "stringutils.h"
#include "triangulate.h"
#include "gui.h"
#include "yxml.h"

#include "coco.h"

static rgba_t default_group_color = RGBA(60, 220, 50, 255);

#include "annotation_asap_xml.cpp"

i32 add_annotation_group(annotation_set_t* annotation_set, const char* name) {
	annotation_group_t new_group = {};
    new_group.color = default_group_color; // default color
	strncpy(new_group.name, name, sizeof(new_group.name));
	arrput(annotation_set->stored_groups, new_group);
	i32 new_stored_group_index = annotation_set->stored_group_count++;

	arrput(annotation_set->active_group_indices, new_stored_group_index);
	i32 new_active_group_index = annotation_set->active_group_count++;

	return new_active_group_index;
}

i32 add_annotation_feature(annotation_set_t* annotation_set, const char* name) {
	annotation_feature_t new_feature = {};
	strncpy(new_feature.name, name, sizeof(new_feature.name));
	i32 new_stored_feature_index = annotation_set->stored_feature_count++;
	new_feature.id = new_stored_feature_index;
	arrput(annotation_set->stored_features, new_feature);

	arrput(annotation_set->active_feature_indices, new_stored_feature_index);
	i32 new_active_feature_index = annotation_set->active_feature_count++;

	return new_active_feature_index;
}

i32 find_annotation_group(annotation_set_t* annotation_set, const char* group_name) {
	for (i32 i = 0; i < annotation_set->stored_group_count; ++i) {
		if (strcmp(annotation_set->stored_groups[i].name, group_name) == 0) {
			return i;
		}
	}
	return -1; // not found
}

i32 find_annotation_group_or_create_if_not_found(annotation_set_t* annotation_set, const char* group_name) {
	i32 group_index = find_annotation_group(annotation_set, group_name);
	if (group_index < 0) {
		group_index = add_annotation_group(annotation_set, group_name); // Group not found --> create it
	}
	return group_index;
}

i32 find_annotation_feature(annotation_set_t* annotation_set, const char* feature_name) {
	for (i32 i = 0; i < annotation_set->stored_feature_count; ++i) {
		if (strcmp(annotation_set->stored_features[i].name, feature_name) == 0) {
			return i;
		}
	}
	return -1; // not found
}

i32 find_annotation_feature_or_create_if_not_found(annotation_set_t* annotation_set, const char* feature_name) {
	i32 feature_index = find_annotation_feature(annotation_set, feature_name);
	if (feature_index < 0) {
		feature_index = add_annotation_feature(annotation_set, feature_name); // Feature not found --> create it
	}
	return feature_index;
}


void select_annotation(annotation_set_t* annotation_set, annotation_t* annotation) {
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* a = get_active_annotation(annotation_set, i);
		a->selected = false;
	}
	annotation->selected = true;
}

void deselect_all_annotations(annotation_set_t* annotation_set) {
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* annotation = get_active_annotation(annotation_set, i);
		annotation->selected = false;
	}
}


void annotation_set_rectangle_coordinates_to_bounding_box(annotation_set_t* annotation_set, annotation_t* annotation) {
	if (annotation->coordinate_count == 4) {
		annotation_recalculate_bounds_if_necessary(annotation);
		annotation->coordinates[0] = annotation->bounds.min;
		annotation->coordinates[1] = V2F(annotation->bounds.min.x, annotation->bounds.max.y);
		annotation->coordinates[2] = annotation->bounds.max;
		annotation->coordinates[3] = V2F(annotation->bounds.max.x, annotation->bounds.min.y);
		annotation_invalidate_derived_calculations_from_coordinates(annotation);
		notify_annotation_set_modified(annotation_set);
	}
}

void do_drag_annotation_node(scene_t* scene) {
	annotation_set_t* annotation_set = &scene->annotation_set;
	i32 annotation_with_selected_coordinate = annotation_set->selected_coordinate_annotation_index;
	if (annotation_with_selected_coordinate >= 0) {
		annotation_t* annotation = get_active_annotation(&scene->annotation_set, annotation_set->selected_coordinate_annotation_index);
		i32 coordinate_index = annotation_set->selected_coordinate_index;
		if (coordinate_index >= 0 && coordinate_index < annotation->coordinate_count) {
			// Update the coordinate(s)
			v2f* coordinate = annotation->coordinates + coordinate_index;
			coordinate->x = scene->mouse.x - annotation_set->coordinate_drag_start_offset.x;
			coordinate->y = scene->mouse.y - annotation_set->coordinate_drag_start_offset.y;
			if (annotation->type == ANNOTATION_RECTANGLE && annotation->coordinate_count == 4) {
				// Fixup rectangle coordinates
				i32 fixed_coordinate_index = (coordinate_index + 2) % 4;
				v2f fixed = annotation->coordinates[fixed_coordinate_index];
				annotation->coordinates[(coordinate_index + 1) % 4] = V2F(fixed.x, coordinate->y);
				annotation->coordinates[(coordinate_index + 3) % 4] = V2F(coordinate->x, fixed.y);
			}
			annotation_invalidate_derived_calculations_from_coordinates(annotation);
			notify_annotation_set_modified(&scene->annotation_set);

		}
	}
}

void annotation_set_automatic_name(annotation_t* annotation, i32 annotation_index) {
	snprintf(annotation->name, sizeof(annotation->name), "Annotation %d", annotation_index);
}

void create_ellipse_annotation(annotation_set_t* annotation_set, v2f pos) {
	annotation_t new_annotation = {};
	new_annotation.type = ANNOTATION_ELLIPSE;
	new_annotation.p0 = pos;
	new_annotation.p1 = pos;
	new_annotation.bounds = {};
	new_annotation.bounds.min = pos;
	new_annotation.bounds.max = pos;
	new_annotation.group_id = annotation_set->last_assigned_annotation_group;

	deselect_all_annotations(annotation_set);
	new_annotation.selected = true;

	i32 active_index = annotation_set->active_annotation_count;
	annotation_set_automatic_name(&new_annotation, active_index);

	arrput(annotation_set->stored_annotations, new_annotation);
	i32 annotation_stored_index = annotation_set->stored_annotation_count++;

	arrput(annotation_set->active_annotation_indices, annotation_stored_index);
	annotation_set->active_annotation_count++;

	annotation_set->editing_annotation_index = active_index;

	notify_annotation_set_modified(annotation_set);

	// Creating an annotation implies that you might want to edit it as well
	annotation_set->is_edit_mode = true;
}

void finalize_ellipse_annotation(annotation_set_t* annotation_set, annotation_t* annotation, v2f pos) {
	ASSERT(annotation->type == ANNOTATION_ELLIPSE);
	annotation->p1 = pos;
	// TODO: recalculate bounds

	if (annotation->coordinate_count > 0) {
		// ?
	} else {
		// TODO
	}

}

void create_rectangle_annotation(annotation_set_t* annotation_set, v2f pos) {
	annotation_t new_annotation = {};
	new_annotation.type = ANNOTATION_RECTANGLE;
	new_annotation.p0 = pos;
	new_annotation.p1 = pos;
	new_annotation.bounds = {};
	new_annotation.bounds.min = pos;
	new_annotation.bounds.max = pos;
	new_annotation.group_id = annotation_set->last_assigned_annotation_group;

	new_annotation.coordinate_count = 4;
	arrsetlen(new_annotation.coordinates, 4);
	for (i32 i = 0; i < 4; ++i) {
		new_annotation.coordinates[i] = pos;
	}

	deselect_all_annotations(annotation_set);
	new_annotation.selected = true;

	i32 active_index = annotation_set->active_annotation_count;
	annotation_set_automatic_name(&new_annotation, active_index);

	arrput(annotation_set->stored_annotations, new_annotation);
	i32 annotation_stored_index = annotation_set->stored_annotation_count++;

	arrput(annotation_set->active_annotation_indices, annotation_stored_index);
	i32 annotation_active_index = annotation_set->active_annotation_count++;

	// Select the first point for dragging
	annotation_set->selected_coordinate_index = 0;
	annotation_set->selected_coordinate_annotation_index = annotation_active_index;

	notify_annotation_set_modified(annotation_set);

	annotation_set->editing_annotation_index = active_index;

	// Creating an annotation implies that you might want to edit it as well
	annotation_set->is_edit_mode = true;
}

void do_mouse_tool_create_rectangle(app_state_t* app_state, input_t* input, scene_t* scene, annotation_set_t* annotation_set) {
	if (scene->drag_started) {
		create_rectangle_annotation(annotation_set, scene->mouse);
	} else if (scene->is_dragging) {
		do_drag_annotation_node(scene);
	} else if (scene->drag_ended) {
		// finalize shape
		viewer_switch_tool(app_state, TOOL_NONE);
	} else if (was_key_pressed(input, KEY_Escape)) {
		viewer_switch_tool(app_state, TOOL_NONE);
	}
}

void create_freeform_annotation(annotation_set_t* annotation_set, v2f pos) {
	annotation_t new_annotation = {};
	new_annotation.type = ANNOTATION_POLYGON;
	new_annotation.p0 = pos;
	new_annotation.p1 = pos;
	new_annotation.bounds = {};
	new_annotation.bounds.min = pos;
	new_annotation.bounds.max = pos;
	new_annotation.group_id = annotation_set->last_assigned_annotation_group;

	new_annotation.coordinate_count = 1;
	arrsetlen(new_annotation.coordinates, 1);
	new_annotation.coordinates[0] = pos;

	// This annotation is 'unfinished', don't draw the completing line back to the start point
	new_annotation.is_open = true;

	deselect_all_annotations(annotation_set);
	new_annotation.selected = true;

	i32 active_index = annotation_set->active_annotation_count;
	annotation_set_automatic_name(&new_annotation, active_index);

	arrput(annotation_set->stored_annotations, new_annotation);
	i32 annotation_stored_index = annotation_set->stored_annotation_count++;

	arrput(annotation_set->active_annotation_indices, annotation_stored_index);
	i32 annotation_active_index = annotation_set->active_annotation_count++;

	// Select the first point for dragging
	annotation_set->selected_coordinate_index = 0;
	annotation_set->selected_coordinate_annotation_index = annotation_active_index;

	notify_annotation_set_modified(annotation_set);

	annotation_set->editing_annotation_index = active_index;

	// Creating an annotation implies that you might want to edit it as well
	annotation_set->is_edit_mode = true;
}

void do_mouse_tool_create_freeform(app_state_t* app_state, input_t* input, scene_t* scene, annotation_set_t* annotation_set) {
	if (annotation_set->editing_annotation_index < 0) {
		// Start a new freeform
		if (was_key_pressed(input, KEY_Escape)) {
			viewer_switch_tool(app_state, TOOL_NONE);
		} else if (scene->drag_started) {
			create_freeform_annotation(&scene->annotation_set, scene->mouse);
		}
	} else {
		// Continue adding to an already-existing freeform
		annotation_t* freeform = get_active_annotation(annotation_set,
		                                               annotation_set->editing_annotation_index);
		if (was_key_pressed(input, KEY_Escape)) {
			// Abort
			if (freeform) {
				if (freeform->coordinate_count >= 3) {
					// Abort method 1: finalize
					freeform->is_open = false;
					notify_annotation_set_modified(annotation_set);
				} else {
					// Abort method 2: delete (if not enough coordinates were already added)
					delete_annotation(annotation_set, annotation_set->editing_annotation_index);
				}
			}
			viewer_switch_tool(app_state, TOOL_NONE);
		} else if (freeform->type == ANNOTATION_POLYGON && freeform->coordinate_count > 0) {
			// Add new points to 'in-progress' freeform annotation
			v2f start = freeform->coordinates[0];
			i32 last_coordinate_index = freeform->coordinate_count - 1;
			v2f last = freeform->coordinates[last_coordinate_index];
			float distance_since_last_point = v2f_length(v2f_subtract(scene->mouse, last));
			distance_since_last_point /= scene->zoom.screen_point_width;

			float distance_to_start_point = v2f_length(v2f_subtract(scene->mouse, start));
			distance_to_start_point /= scene->zoom.screen_point_width;

			if (was_key_pressed(input, KEY_C)) {
				// delete the last coordinate
				if (freeform->coordinate_count > 1) {
					arrpop(freeform->coordinates);
					--freeform->coordinate_count;
				} else {
					// trying to delete the last coordinate -> abort instead
					delete_annotation(annotation_set, annotation_set->editing_annotation_index);
					viewer_switch_tool(app_state, TOOL_NONE);
				}
			}

			if (scene->drag_started) {
				if (distance_to_start_point < annotation_hover_distance && freeform->coordinate_count >= 3) {
					// finalize the annotation
					freeform->is_open = false;
					annotation_invalidate_derived_calculations_from_coordinates(freeform);
					notify_annotation_set_modified(annotation_set);
					viewer_switch_tool(app_state, TOOL_NONE);
					scene->drag_started = false;
					scene->is_dragging = false;
					// Prevent click being registered next frame (annotation might get deselected otherwise).
					scene->suppress_next_click = true;
				} else {
					// add a point
					arrput(freeform->coordinates, scene->mouse);
					++freeform->coordinate_count;
				}
			} else if (scene->is_dragging) {
				// drop points along the way
				if (distance_since_last_point > annotation_freeform_insert_interval_distance && distance_to_start_point > annotation_hover_distance) {
					arrput(freeform->coordinates, scene->mouse);
					++freeform->coordinate_count;
				}
			} else if (scene->drag_ended) {

				if (distance_to_start_point < annotation_hover_distance && freeform->coordinate_count >= 3) {
					// finalize the annotation
					freeform->is_open = false;
					annotation_invalidate_derived_calculations_from_coordinates(freeform);
					viewer_switch_tool(app_state, TOOL_NONE);
					notify_annotation_set_modified(annotation_set);
				}
			}
		} else {
			ASSERT(!"invalid code path");
		}
	}
}

void create_line_annotation(annotation_set_t* annotation_set, v2f pos) {
	annotation_t new_annotation = {};
	new_annotation.type = ANNOTATION_POLYGON;
	new_annotation.p0 = pos;
	new_annotation.p1 = pos;
	new_annotation.bounds = {};
	new_annotation.bounds.min = pos;
	new_annotation.bounds.max = pos;

	new_annotation.group_id = annotation_set->last_assigned_annotation_group;
	new_annotation.coordinate_count = 2;
	arrsetlen(new_annotation.coordinates, 2);
	new_annotation.coordinates[0] = pos;
	new_annotation.coordinates[1] = pos;

	deselect_all_annotations(annotation_set);
	new_annotation.selected = true;

	i32 active_index = annotation_set->active_annotation_count;
	annotation_set_automatic_name(&new_annotation, active_index);

	arrput(annotation_set->stored_annotations, new_annotation);
	i32 annotation_stored_index = annotation_set->stored_annotation_count++;

	arrput(annotation_set->active_annotation_indices, annotation_stored_index);
	i32 annotation_active_index = annotation_set->active_annotation_count++;

	// Select the second point for dragging
	annotation_set->selected_coordinate_index = new_annotation.coordinate_count - 1;
	annotation_set->selected_coordinate_annotation_index = annotation_active_index;

	notify_annotation_set_modified(annotation_set);

	// Creating an annotation implies that you might want to edit it as well
	annotation_set->is_edit_mode = true;
}

void do_mouse_tool_create_line(app_state_t* app_state, input_t* input, scene_t* scene, annotation_set_t* annotation_set) {
	if (scene->drag_started) {
		create_line_annotation(&scene->annotation_set, scene->mouse);
		viewer_switch_tool(app_state, TOOL_NONE);
		app_state->mouse_mode = MODE_DRAG_ANNOTATION_NODE;
//							console_print("Creating a line\n");
	} else if (was_key_pressed(input, KEY_Escape)) {
		viewer_switch_tool(app_state, TOOL_NONE);
	}
}

void create_point_annotation(annotation_set_t* annotation_set, v2f pos) {

	annotation_t new_annotation = {};
	new_annotation.type = ANNOTATION_POINT;
	new_annotation.p0 = pos;
	new_annotation.bounds = {};
	new_annotation.bounds.min = pos;
	new_annotation.bounds.max = pos;
//		new_annotation.name;
	new_annotation.group_id = annotation_set->last_assigned_annotation_group;
	new_annotation.coordinate_count = 1;
	arrput(new_annotation.coordinates, pos);

	deselect_all_annotations(annotation_set);
	new_annotation.selected = true;

	i32 active_index = annotation_set->active_annotation_count;
	annotation_set_automatic_name(&new_annotation, active_index);

	arrput(annotation_set->stored_annotations, new_annotation);
	i32 annotation_stored_index = annotation_set->stored_annotation_count++;

	arrput(annotation_set->active_annotation_indices, annotation_stored_index);
	annotation_set->active_annotation_count++;

	notify_annotation_set_modified(annotation_set);

	// Creating an annotation implies that you might want to edit it as well
	annotation_set->is_edit_mode = true;
}

// TODO: think about what we want to do with this functionality; remove?
bool want_cycle_annotations = false;

void interact_with_annotations(app_state_t* app_state, scene_t* scene, input_t* input) {
	annotation_set_t* annotation_set = &scene->annotation_set;

	// Pressing Tab or Shift+Tab cycles annotations within the currently selected group
    if (want_cycle_annotations) {
        if (!gui_want_capture_keyboard && was_key_pressed(input, KEY_Tab)) {
            i32 delta = (is_key_down(input, KEY_LeftShift) || is_key_down(input, KEY_RightShift)) ? -1 : +1;
            i32 selected_index = annotation_cycle_selection_within_group(&app_state->scene.annotation_set, delta);
            if (selected_index >= 0) {
                center_scene_on_annotation(&app_state->scene, get_active_annotation(annotation_set, selected_index));
            }
        }
    }

	// Pressing Escape deselects annotations
	if (!gui_want_capture_keyboard && was_key_pressed(input, KEY_Escape)) {
		deselect_all_annotations(annotation_set);
	}

	// Pressing E toggles coordinate editing mode
	if (!gui_want_capture_keyboard && was_key_pressed(input, KEY_E)) {
		annotation_set->is_edit_mode = !annotation_set->is_edit_mode;
	}

	// Determine which annotation is being targeted by the mouse.
	annotation_hit_result_t hit_result = {};
	bool need_select_deselect = false;
	if (annotation_set->is_edit_mode) {
		// For most edit operations, including those targeting a coordinate node, we want to be biased toward the
		// selected annotation (otherwise we might end up interacting with a non-selected annotation instead)
		hit_result = get_annotation_hit_result(annotation_set, scene->mouse,
											   300.0f * scene->zoom.screen_point_width,
											   +5.0f * scene->zoom.screen_point_width);

		if (!annotation_set->is_insert_coordinate_mode && !annotation_set->is_insert_coordinate_mode) {
			// In this case we can either try to grab a coordinate node, or select/deselect annotation.
			if ((hit_result.coordinate_distance / scene->zoom.screen_point_width) > annotation_hover_distance) {
				// Apparently we are 'out of range' for targeting a coordinate node
				// --> fall back to targeting a whole annotation for select/deselect
				need_select_deselect = true;
			}
		}
	}
	if (!annotation_set->is_edit_mode || need_select_deselect) {
		// NOTE: There is a small negative bias for clicking on selected annotations, so that you are more
		// likely to switch over to another annotation instead of deselecting the one you are on.
		hit_result = get_annotation_hit_result(annotation_set, scene->mouse,
		                                       300.0f * scene->zoom.screen_point_width,
		                                       -5.0f * scene->zoom.screen_point_width);
	}

	if (hit_result.is_valid) {
		ASSERT(scene->zoom.screen_point_width > 0.0f);
		float line_segment_pixel_distance = hit_result.line_segment_distance / scene->zoom.screen_point_width;
		float coordinate_pixel_distance = hit_result.coordinate_distance / scene->zoom.screen_point_width;

		annotation_t* hit_annotation = get_active_annotation(annotation_set, hit_result.annotation_index);
		v2f* hit_coordinate = hit_annotation->coordinates + hit_result.coordinate_index;
		annotation_set->hovered_annotation = hit_result.annotation_index;
		annotation_set->hovered_coordinate = hit_result.coordinate_index;
		annotation_set->hovered_coordinate_pixel_distance = coordinate_pixel_distance;

		bool click_action_handled = false;

		if (annotation_set->is_edit_mode) {

			// The special mode for inserting new coordinate is enabled under one of two conditions:
			// - While editing, the user uses Shift+Click to quickly insert a new coordinate between two coordinates
			//   (if you release Shift without clicking, the mode is disabled again)
			// - While editing, the user right-clicks, selects the context menu item for inserting a coordinate
			//   and enters 'forced' insert mode. This mode ends when you click
			if (!gui_want_capture_mouse &&
					((app_state->mouse_mode == MODE_VIEW && input->keyboard.key_shift.down) || annotation_set->force_insert_mode)) {
				annotation_set->is_insert_coordinate_mode = true;
			} else {
				annotation_set->is_insert_coordinate_mode = false;
			}

			// Actions for clicking (LMB down) and/or starting a dragging operation while in editing mode:
			// - Basic action: if we are close enough to a coordinate, we 'grab' the coordinate and start dragging it.
			// - If we are in insert coordinate mode, we instead create a new coordinate at the chosen location
			//   and then immediately start dragging the new coordinate.
			if (scene->drag_started && !annotation_set->is_split_mode && (hit_annotation->selected || hit_annotation->type == ANNOTATION_POINT)) {
				if (annotation_set->is_insert_coordinate_mode) {
					// check if we have clicked close enough to a line segment to insert a coordinate there
					v2f projected_point = {};
					float distance_to_edge = FLT_MAX;
					float t_clamped = 0.0f;
					i32 insert_at_index = project_point_onto_annotation(annotation_set, hit_annotation,
					                                                    app_state->scene.mouse, &t_clamped,
					                                                    &projected_point, &distance_to_edge);
					if (insert_at_index >= 0) {
						float pixel_distance_to_edge = distance_to_edge / scene->zoom.screen_point_width;
						if (pixel_distance_to_edge < annotation_insert_hover_distance) {
							// try to insert a new coordinate, and start dragging that
							insert_coordinate(app_state, annotation_set, hit_annotation, insert_at_index, projected_point);
							// update state so we can immediately interact with the newly created coordinate
							annotation_set->is_insert_coordinate_mode = false;
							annotation_set->force_insert_mode = false;
							hit_result.coordinate_distance = coordinate_pixel_distance = 0.0f;
							hit_result.coordinate_index = insert_at_index;
							hit_result.line_segment_coordinate_index = insert_at_index;
							hit_result.line_segment_distance = distance_to_edge;
							hit_result.line_segment_t_clamped = t_clamped;
							hit_result.line_segment_projected_point = projected_point;
							hit_coordinate = hit_annotation->coordinates + hit_result.coordinate_index;
							annotation_set->hovered_coordinate = hit_result.coordinate_index;
							annotation_set->hovered_coordinate_pixel_distance = coordinate_pixel_distance;
							select_annotation(annotation_set, hit_annotation);
						}
					}
				}

				// Start dragging a coordinate
				if (coordinate_pixel_distance < annotation_hover_distance) {
//				    console_print("Grabbed coordinate %d\n", nearest_coordinate_index);
					app_state->mouse_mode = MODE_DRAG_ANNOTATION_NODE;
					annotation_set->selected_coordinate_index = hit_result.coordinate_index;
					annotation_set->selected_coordinate_annotation_index = hit_result.annotation_index;
					annotation_set->coordinate_drag_start_offset.x = scene->mouse.x - hit_coordinate->x;
					annotation_set->coordinate_drag_start_offset.y = scene->mouse.y - hit_coordinate->y;
				}
			}

			// Actions when scene is clicked (LMB release without dragging):
			// - Select a previously unselected annotation
			// - Unselect an annotation
			// - Special: when in splitting mode and clicking on a coordinate, split that annotation in two.
			if (scene->clicked) {

				// Annotation splitting: if we are in split mode, then another coordinate was selected earlier;
				// in this case we should try to finish the splitting operation by connecting up the newly selected coordinate
				if (annotation_set->is_split_mode) {
					bool split_ok = false;
					// Have we clicked on a coordinate?
					if (hit_annotation->selected && coordinate_pixel_distance < annotation_hover_distance) {
						// Are the coordinates valid?
						if (coordinate_index_valid_for_annotation(annotation_set->selected_coordinate_index, hit_annotation) &&
						    coordinate_index_valid_for_annotation(hit_result.coordinate_index, hit_annotation))
						{
							split_annotation(app_state, annotation_set, hit_annotation, annotation_set->selected_coordinate_index, hit_result.coordinate_index);
							// TODO: pointer might have changed! better solution?
							// Refresh pointer (might have relocated if the array was realloced due adding a new annotation)
							hit_annotation = get_active_annotation(annotation_set, hit_result.annotation_index);
							split_ok = true;
						}
					}
					if (!split_ok) {
						// failed
					}
					click_action_handled = true;
				}

				// Have we clicked on a coordinate?
				if (hit_annotation->selected && coordinate_pixel_distance < annotation_hover_distance) {
					annotation_set->selected_coordinate_index = hit_result.coordinate_index;
					annotation_set->selected_coordinate_annotation_index = hit_result.annotation_index;
				}

			}
		} else {
			// We are not in editing mode.
			deselect_annotation_coordinates(annotation_set);
			annotation_set->is_insert_coordinate_mode = false;
			annotation_set->force_insert_mode = false;
			annotation_set->is_split_mode = false;
		}
		// Select/unselect (both in edit mode and not in edit mode)
		if (scene->clicked && !click_action_handled) {
			bool did_select = false;

			if (!click_action_handled) {
				if (!hit_annotation->selected) {
					hit_annotation->selected = true;
					did_select = true;
				} else {
					hit_annotation->selected = false;
				}
				deselect_annotation_coordinates(annotation_set);
				click_action_handled = true;
			}

			ASSERT(click_action_handled);

			// Feature for quickly assigning the same annotation group to the next selected annotation.
			if (did_select && hit_annotation->selected && auto_assign_last_group && annotation_set->last_assigned_group_is_valid) {
				hit_annotation->group_id = annotation_set->last_assigned_annotation_group;
				notify_annotation_set_modified(annotation_set);
			}

			annotation_set->is_insert_coordinate_mode = false;
			annotation_set->force_insert_mode = false;
			annotation_set->is_split_mode = false;
		}

	}

	// Drop a new dot annotation under the mouse cursor position
	if (!scene->is_dragging && annotation_set->is_edit_mode && was_key_pressed(input, KEY_Q)) {
		create_point_annotation(annotation_set, scene->mouse);
	}

	// unselect all annotations (except if Ctrl held down)
	if (scene->clicked && !input->keyboard.key_ctrl.down) {
		for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
			if (i == hit_result.annotation_index) continue; // skip the one we just selected!
			annotation_t* annotation = get_active_annotation(annotation_set, i);
			annotation->selected = false;
		}
	}

	// Multi-select by holding down Ctrl and dragging
	if (scene->is_dragging && input->keyboard.key_ctrl.down && !scene->is_drag_vector_within_click_tolerance) {
		if (hit_result.line_segment_distance < 10.0f) {
			annotation_t* annotation = get_active_annotation(annotation_set, hit_result.annotation_index);
			annotation->selected = true;
		}
	}

	recount_selected_annotations(app_state, annotation_set);

	if (annotation_set->selection_count > 0) {
		if (!gui_want_capture_keyboard) {
			if (was_key_pressed(input, KEY_DeleteForward)) {
				if (hit_result.annotation_index >= 0) {
					if (dont_ask_to_delete_annotations) {
						delete_selected_annotations(app_state, annotation_set);
					} else {
						show_delete_annotation_prompt = true;
					}
				}
			}

			// Delete a coordinate by pressing 'C' while hovering over the coordinate
			static i64 key_hold_down_time_start;
			static i32 repeat_counter;
			if (was_key_pressed(input, KEY_C)) {
				key_hold_down_time_start = get_clock();
				repeat_counter = 0;
			}
			if (is_key_down(input, KEY_C) && annotation_set->is_edit_mode) {
				// can delete a coordinate every frame, but only after the initial 0.25 s delay
				if (repeat_counter == 0 || get_seconds_elapsed(key_hold_down_time_start, get_clock()) > 0.25f) {
					++repeat_counter;
					if (hit_result.annotation_index >= 0 && annotation_set->hovered_coordinate >= 0 && annotation_set->hovered_coordinate_pixel_distance < annotation_hover_distance) {
						annotation_t* hit_annotation = get_active_annotation(annotation_set, hit_result.annotation_index);
						if (hit_annotation->selected) {
							delete_coordinate(annotation_set, hit_result.annotation_index, annotation_set->hovered_coordinate);
						}
					}
				}
			}
		}
	} else {
		// nothing selected
		annotation_set->force_insert_mode = false;
	}

}

// Create a 2D bounding box that encompasses all of the annotation's coordinates
bounds2f bounds_for_annotation(annotation_t* annotation) {
	bounds2f result = { +FLT_MAX, +FLT_MAX, -FLT_MAX, -FLT_MAX };
	if (annotation->coordinate_count >=1) {
		for (i32 i = 0; i < annotation->coordinate_count; ++i) {
			v2f* coordinate = annotation->coordinates + i;
			float x = (float)coordinate->x;
			float y = (float)coordinate->y;
			result.left = MIN(result.left, x);
			result.right = MAX(result.right, x);
			result.top = MIN(result.top, y);
			result.bottom = MAX(result.bottom, y);
		}
	}
	return result;
}

void annotation_recalculate_bounds_if_necessary(annotation_t* annotation) {
	if (!(annotation->valid_flags & ANNOTATION_VALID_BOUNDS)) {
		annotation->bounds = bounds_for_annotation(annotation);
		annotation->valid_flags |= ANNOTATION_VALID_BOUNDS;
	}
}

bool is_point_within_annotation_bounds(annotation_t* annotation, v2f point, float tolerance_margin) {
	annotation_recalculate_bounds_if_necessary(annotation);
	bounds2f bounds = annotation->bounds;
	// TODO: Maybe make the tolerance depend on the size of the annotation?
	if (tolerance_margin != 0.0f) {
		bounds.left -= tolerance_margin;
		bounds.right += tolerance_margin;
		bounds.top -= tolerance_margin;
		bounds.bottom += tolerance_margin;
	}
	bool result = v2f_within_bounds(bounds, point);
	return result;
}

// Determine which annotation & coordinate are closest to a certain point in space (e.g., the mouse cursor position)
annotation_hit_result_t get_annotation_hit_result(annotation_set_t* annotation_set, v2f point, float bounds_check_tolerance, float bias_for_selected) {
	annotation_hit_result_t hit_result = {};
	hit_result.annotation_index = -1;
	hit_result.line_segment_coordinate_index = -1;
	hit_result.line_segment_distance = FLT_MAX;
	hit_result.coordinate_index = -1;
	hit_result.coordinate_distance = FLT_MAX;
	hit_result.is_valid = false;

	// TODO: make a way to only check against selected annotations, ignoring others entirely?
	float shortest_biased_line_segment_distance = FLT_MAX; // for making it easier to focus on only selected annotations

	// Finding the nearest annotation
	// Step 1: discard annotation if the point is outside the annotation's min/max coordinate bounds (plus a tolerance margin)
	// Step 2: for the remaining annotations, calculate the distances from the point to each of the line segments between coordinates.
	// Step 3: choose the annotation that has the closest distance.
	for (i32 annotation_index = 0; annotation_index < annotation_set->active_annotation_count; ++annotation_index) {
		annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);
		// TODO: think about what to do for annotations that are not polygons (e.g., circles) / i.e. have no coordinates
		if (annotation->coordinate_count > 0) {
			if (is_point_within_annotation_bounds(annotation, point, bounds_check_tolerance)) {
				float bias = annotation->selected ? bias_for_selected : 0.0f;
				float line_segment_distance = FLT_MAX; // distance to line segment
				v2f projected_point = {}; // projected point on line segment
				float t_clamped = 0.0f; // how for we are along the line segment (between 0 and 1)
				i32 nearest_line_segment_coordinate_index = project_point_onto_annotation(annotation_set, annotation,
				                                                                          point, &t_clamped,
				                                                                          &projected_point,
				                                                                          &line_segment_distance);
				float biased_line_segment_distance = line_segment_distance - bias;
				if (nearest_line_segment_coordinate_index >= 0 && biased_line_segment_distance < shortest_biased_line_segment_distance) {
					shortest_biased_line_segment_distance = biased_line_segment_distance;
					hit_result.line_segment_distance = line_segment_distance;
					hit_result.line_segment_coordinate_index = nearest_line_segment_coordinate_index;
					hit_result.line_segment_projected_point = projected_point;
					hit_result.line_segment_t_clamped = t_clamped;
					hit_result.annotation_index = annotation_index;
				}
			}
		}

	}

	// Step 4: determine the closest coordinate of the closest annotation (as calculated above)
	// Note: this is not necessarily the same as the closest coordinate globally (that may belong to a different annotation)!
	float nearest_coordinate_distance_sq = FLT_MAX;
	if (hit_result.annotation_index >= 0) {
		annotation_t* annotation = get_active_annotation(annotation_set, hit_result.annotation_index);
		// TODO: what about annotations that don't have coordinates?
		ASSERT(annotation->coordinate_count > 0);
		for (i32 i = 0; i < annotation->coordinate_count; ++i) {
			v2f* coordinate = annotation->coordinates + i;
			float delta_x = point.x - coordinate->x;
			float delta_y = point.y - coordinate->y;
			float sq_distance = SQUARE(delta_x) + SQUARE(delta_y);
			if (sq_distance < nearest_coordinate_distance_sq) {
				nearest_coordinate_distance_sq = sq_distance;
				hit_result.coordinate_index = i;
				hit_result.coordinate_distance = sqrtf(nearest_coordinate_distance_sq);
				hit_result.is_valid = true;
			}
		}
	}

	return hit_result;
}


i32 project_point_onto_annotation(annotation_set_t* annotation_set, annotation_t* annotation, v2f point, float* t_ptr, v2f* projected_point_ptr, float* distance_ptr) {
	i32 insert_before_index = -1;
	ASSERT(annotation->coordinate_count > 0);
	if (annotation->coordinate_count == 1) {
		// trivial case
		v2f line_point = annotation->coordinates[0];
		if (t_ptr) *t_ptr = 0.0f;
		if (projected_point_ptr) *projected_point_ptr = line_point;
		if (distance_ptr) {
			float distance = v2f_length(v2f_subtract(point, line_point));
			*distance_ptr = distance;
		}
		insert_before_index = 1;
	} else if (annotation->coordinate_count > 1) {
		// find the line segment (between coordinates) closest to the point we are checking against
		float closest_distance_sq = FLT_MAX;
		v2f closest_projected_point = {};
		float t_closest = 0.0f;
		bool found_closest = false;
		for (i32 i = 0; i < annotation->coordinate_count; ++i) {
			v2f* coordinate_current = annotation->coordinates + i;
			i32 coordinate_index_after = (i + 1) % annotation->coordinate_count;
			v2f* coordinate_after = annotation->coordinates + coordinate_index_after;
			v2f line_start = *coordinate_current;
			v2f line_end = *coordinate_after;
			float t = 0.0f;
			v2f projected_point = project_point_on_line_segment(point, line_start, line_end, &t);
			float distance_sq = v2f_length_squared(v2f_subtract(point, projected_point));
			if (distance_sq < closest_distance_sq) {
				found_closest = true;
				closest_distance_sq = distance_sq;
				closest_projected_point = projected_point;
				insert_before_index = coordinate_index_after;
			}
		}
		ASSERT(found_closest);
		if (found_closest) {
			if (projected_point_ptr) {
				*projected_point_ptr = closest_projected_point;
			}
			if (distance_ptr) {
				float closest_distance = sqrtf(closest_distance_sq);
				*distance_ptr = closest_distance;
			}
		}
	}
	return insert_before_index;
}

void deselect_annotation_coordinates(annotation_set_t* annotation_set) {
	annotation_set->selected_coordinate_index = -1;
	annotation_set->selected_coordinate_annotation_index = -1;
}

void notify_annotation_set_modified(annotation_set_t* annotation_set) {
	annotation_set->modified = true; // need to (auto-)save the changes
	annotation_set->last_modification_time = get_clock();
}

void annotation_invalidate_derived_calculations_from_coordinates(annotation_t* annotation) {
	u32 derived_calculation_flags_mask = (ANNOTATION_VALID_BOUNDS | ANNOTATION_VALID_TESSELATION | ANNOTATION_VALID_AREA | ANNOTATION_VALID_LENGTH);
	annotation->fallback_valid_flags |= (annotation->valid_flags & derived_calculation_flags_mask);
	annotation->valid_flags &= ~(derived_calculation_flags_mask);
}

void annotation_invalidate_derived_calculations_from_features(annotation_t* annotation) {
	u32 derived_calculation_flags_mask = (ANNOTATION_VALID_NONZERO_FEATURE_COUNT);
//	annotation->fallback_valid_flags |= (annotation->valid_flags & derived_calculation_flags_mask);
	annotation->valid_flags &= ~(derived_calculation_flags_mask);
}

bool maybe_change_annotation_type_based_on_coordinate_count(annotation_t* annotation) {
	bool changed = false;
	ASSERT(annotation->coordinate_count >= 1);
	if (annotation->type == ANNOTATION_RECTANGLE) {
		if (annotation->coordinate_count != 4) {
			annotation->type = ANNOTATION_POLYGON;
			changed = true;
		}
	}
	if (annotation->type == ANNOTATION_POINT) {
		if (annotation->coordinate_count >= 2) {
			if (annotation->coordinate_count == 2) {
				annotation->type = ANNOTATION_LINE;
			} else {
				annotation->type = ANNOTATION_POLYGON;
			}
			changed = true;
		}
	} else if (annotation->type == ANNOTATION_LINE) {
		if (annotation->coordinate_count != 2) {
			if (annotation->coordinate_count == 1) {
				annotation->type = ANNOTATION_POINT;
			} else if (annotation->coordinate_count > 2) {
				annotation->type = ANNOTATION_POLYGON;
			}
			changed = true;
		}
	} else if (annotation->type == ANNOTATION_POLYGON || annotation->type == ANNOTATION_SPLINE) {
		if (annotation->coordinate_count < 3) {
			if (annotation->coordinate_count == 2) {
				annotation->type = ANNOTATION_LINE;
			} else {
				annotation->type = ANNOTATION_POINT;
			}
			changed = true;
		}
	}
	return changed;
}

void insert_coordinate(app_state_t* app_state, annotation_set_t* annotation_set, annotation_t* annotation, i32 insert_at_index, v2f new_coordinate) {
	if (insert_at_index >= 0 && insert_at_index <= annotation->coordinate_count) {
		arrins(annotation->coordinates, insert_at_index, new_coordinate);
		++annotation->coordinate_count;

		// The coordinate count has changed, maybe the type needs to change?
		maybe_change_annotation_type_based_on_coordinate_count(annotation);

		annotation_invalidate_derived_calculations_from_coordinates(annotation);
		notify_annotation_set_modified(annotation_set);
//		console_print("inserted a coordinate at index %d\n", insert_at_index);
	} else {
#if DO_DEBUG
		console_print_error("Error: tried to insert a coordinate at an out of bounds index (%d)\n", insert_at_index);
#endif
	}

}

void delete_coordinate(annotation_set_t* annotation_set, i32 annotation_index, i32 coordinate_index) {
	annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);
	if (coordinate_index >= 0 && coordinate_index < annotation->coordinate_count) {
		if (annotation->coordinate_count == 1) {
			delete_annotation(annotation_set, annotation_index);
		} else {
			arrdel(annotation->coordinates, coordinate_index);
			annotation->coordinate_count--;
			// The coordinate count has changed, maybe the type needs to change?
			maybe_change_annotation_type_based_on_coordinate_count(annotation);

#if 0
			// fixing the order field seems unneeded, the order field is ignored in the XML file anyway.
			for (i32 i = coordinate_index - annotation->first_coordinate; i < annotation->coordinate_count ; ++i) {
				coordinate_t* coordinate = annotation_set->coordinates + annotation->first_coordinate + i;
				coordinate->order = i;
			}
#endif
			annotation_invalidate_derived_calculations_from_coordinates(annotation);
		}
		notify_annotation_set_modified(annotation_set);
		deselect_annotation_coordinates(annotation_set);
	} else {
		panic("coordinate index out of bounds");
	}
}

void annotation_group_delete(annotation_set_t* annotation_set, i32 active_index) {
	ASSERT(active_index >= 0 && active_index < annotation_set->active_group_count);
	i32 stored_index = annotation_set->active_group_indices[active_index];
	ASSERT(stored_index >= 0 && stored_index < annotation_set->stored_group_count);
	annotation_group_t* group = annotation_set->stored_groups + stored_index;
	group->deleted = true;

	i32 default_fallback_group = 0;
	for (i32 i = 0; i < annotation_set->stored_annotation_count; ++i) {
		annotation_t* annotation = annotation_set->stored_annotations + i;
		if (annotation->group_id == stored_index) {
			annotation->group_id = 0;
		}
	}
	arrdel(annotation_set->active_group_indices, active_index);
	--annotation_set->active_group_count;
	notify_annotation_set_modified(annotation_set);
}

void annotation_feature_delete(annotation_set_t* annotation_set, i32 active_index) {
	ASSERT(active_index >= 0 && active_index < annotation_set->active_feature_count);
	i32 stored_index = annotation_set->active_feature_indices[active_index];
	ASSERT(stored_index >= 0 && stored_index < annotation_set->stored_feature_count);
	annotation_feature_t* feature = annotation_set->stored_features + stored_index;
	feature->deleted = true;

	i32 default_fallback_feature = 0;
	for (i32 i = 0; i < annotation_set->stored_annotation_count; ++i) {
		annotation_t* annotation = annotation_set->stored_annotations + i;
		// TODO: reset invalid features
	}
	arrdel(annotation_set->active_feature_indices, active_index);
	--annotation_set->active_feature_count;
	notify_annotation_set_modified(annotation_set);
}

// TODO: delete 'slice' of annotations, instead of hardcoded selected ones
void delete_selected_annotations(app_state_t* app_state, annotation_set_t* annotation_set) {
	if (!annotation_set->stored_annotations) return;
	bool has_selected = false;
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* annotation = get_active_annotation(annotation_set, i);
		if (annotation->selected) {
			has_selected = true;
			break;
		}
	}
	if (has_selected) {
		// rebuild the annotations, leaving out the deleted ones
		temp_memory_t temp_memory = begin_temp_memory_on_local_thread();
		size_t copy_size = annotation_set->active_annotation_count * sizeof(i32);
		i32* temp_copy = (i32*) arena_push_size(temp_memory.arena, copy_size);
		memcpy(temp_copy, annotation_set->active_annotation_indices, copy_size);

		arrsetlen(annotation_set->active_annotation_indices, 0);
		for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
			i32 stored_index = temp_copy[i];
			annotation_t* annotation = annotation_set->stored_annotations + stored_index;
			if (annotation->selected) {
				// TODO: allow undo?
				continue; // skip (delete)
			} else {
				arrput(annotation_set->active_annotation_indices, stored_index);
			}
		}
		annotation_set->active_annotation_count = arrlen(annotation_set->active_annotation_indices);
		notify_annotation_set_modified(annotation_set);
		release_temp_memory(&temp_memory);
	}


}

void delete_annotation(annotation_set_t* annotation_set, i32 active_annotation_index) {
	annotation_t* annotation = get_active_annotation(annotation_set, active_annotation_index);
	destroy_annotation(annotation);
	arrdel(annotation_set->active_annotation_indices, active_annotation_index);
	--annotation_set->active_annotation_count;
}

void split_annotation(app_state_t* app_state, annotation_set_t* annotation_set, annotation_t* annotation, i32 first_coordinate_index, i32 second_coordinate_index) {
	if (first_coordinate_index == second_coordinate_index) {
		// Trivial case: clicked the same coordinate again -> cancel operation
		annotation_set->is_split_mode = false;
		return;
	}
	ASSERT(coordinate_index_valid_for_annotation(first_coordinate_index, annotation));
	ASSERT(coordinate_index_valid_for_annotation(second_coordinate_index, annotation));
	i32 lower_coordinate_index = MIN(first_coordinate_index, second_coordinate_index);
	i32 upper_coordinate_index = MAX(first_coordinate_index, second_coordinate_index);
	if ((upper_coordinate_index - lower_coordinate_index == 1) || (upper_coordinate_index - lower_coordinate_index == annotation->coordinate_count - 1)) {
		// Trivial case: clicked adjacent coordinate -> cancel operation (can't split)
		annotation_set->is_split_mode = false;
		return;
	}

	// step 1: create a new annotation leaving out the section between the lower and upper bounds of the split section
	// Note: the coordinates at the lower and upper bounds themselves are included (duplicated)!
	// e.g.: XX<lower>_____<upper>XXXXX

	// Reserve space for new coord
	annotation_t new_annotation = *annotation;
	i32 new_coordinate_count_lower_part = lower_coordinate_index + 1;
	i32 new_coordinate_count_upper_part = annotation->coordinate_count - upper_coordinate_index;
	new_annotation.coordinate_count = new_coordinate_count_lower_part + new_coordinate_count_upper_part;
	new_annotation.coordinates = NULL;
	arrsetlen(new_annotation.coordinates, new_annotation.coordinate_count);
	v2f* new_coordinates = new_annotation.coordinates;
	v2f* original_coordinates = annotation->coordinates;
	memcpy(&new_coordinates[0],                          &original_coordinates[0],                      new_coordinate_count_lower_part * sizeof(v2f));
	memcpy(&new_coordinates[lower_coordinate_index + 1], &original_coordinates[upper_coordinate_index], new_coordinate_count_upper_part * sizeof(v2f));
	annotation_invalidate_derived_calculations_from_coordinates(&new_annotation);

	// step 2: compactify the original annotation, leaving only the extracted section between the bounds (inclusive)
	// e.g.: __<lower>XXXXX<upper>_____
	i32 new_coordinate_count = upper_coordinate_index - lower_coordinate_index + 1;
	ASSERT(new_coordinate_count >= 3);
	memmove(annotation->coordinates, annotation->coordinates + lower_coordinate_index, new_coordinate_count * sizeof(v2f));
	annotation->coordinate_count = new_coordinate_count;
	annotation_invalidate_derived_calculations_from_coordinates(annotation);
	arrsetlen(annotation->coordinates, new_coordinate_count);

	// Add the new annotation
	i32 new_stored_annotation_index = annotation_set->stored_annotation_count;
	arrput(annotation_set->stored_annotations, new_annotation);
	annotation_set->stored_annotation_count++;
	arrput(annotation_set->active_annotation_indices, new_stored_annotation_index);
	annotation_set->active_annotation_count++;

	annotation_set->is_split_mode = false;
	recount_selected_annotations(app_state, annotation_set);
	notify_annotation_set_modified(annotation_set);
}

void set_group_for_selected_annotations(annotation_set_t* annotation_set, i32 new_group) {
	annotation_set->last_assigned_annotation_group = new_group;
	annotation_set->last_assigned_group_is_valid = true;
	for (i32 i = 0; i < annotation_set->selection_count; ++i) {
		annotation_t* annotation = annotation_set->selected_annotations[i];
		ASSERT(annotation->selected);
		annotation->group_id = new_group;
		notify_annotation_set_modified(annotation_set);
	}
}

void set_type_for_selected_annotations(annotation_set_t* annotation_set, i32 new_type) {
	for (i32 i = 0; i < annotation_set->selection_count; ++i) {
		annotation_t* annotation = annotation_set->selected_annotations[i];
		ASSERT(annotation->selected);
		annotation->type = (annotation_type_enum)new_type;
		notify_annotation_set_modified(annotation_set);
	}
}

void set_features_for_selected_annotations(annotation_set_t* annotation_set, float* features, i32 feature_count) {
	// stub
	for (i32 i = 0; i < annotation_set->selection_count; ++i) {
		annotation_t* annotation = annotation_set->selected_annotations[i];
		ASSERT(annotation->selected);
		memcpy(annotation->features, features, feature_count * sizeof(annotation->features[0]));
		notify_annotation_set_modified(annotation_set);
	}
}

i32 annotation_cycle_selection_within_group(annotation_set_t* annotation_set, i32 delta) {
	if (!(annotation_set->active_annotation_count > 0)) {
		return -1;
	}

	i32 selected_index = 0;
	i32 selected_group = 0;
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* annotation = get_active_annotation(annotation_set, i);
		if (annotation->selected) {
			selected_index = i;
			selected_group = annotation->group_id;
			break;
		}
	}
	bool found_annotation_to_select = false;
	i32 tries = annotation_set->active_annotation_count;
	while (tries--) {
		selected_index += delta;
		while (selected_index < 0) {
			selected_index += annotation_set->active_annotation_count;
		}
		selected_index %= annotation_set->active_annotation_count;
		annotation_t* next = get_active_annotation(annotation_set, selected_index);
		if (next->group_id == selected_group) {
			found_annotation_to_select = true;
			break;
		}
	}

	if (found_annotation_to_select) {
		for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
			annotation_t* annotation = get_active_annotation(annotation_set, i);
			if (i == selected_index) {
				annotation->selected = true;
			} else {
				annotation->selected = false;
			}
		}
		return selected_index;
	} else {
		return -1;
	}

}

void set_region_for_whole_slide(scene_t* scene, image_t* image) {
	// TODO: what to to for offsetted images? (image->origin_offset)
	bounds2f bounds = {0, 0, image->width_in_um, image->height_in_um};
	scene->selection_box = bounds2f_to_rect(bounds);
	scene->crop_bounds = bounds;
	scene->has_selection_box = true;
}

void set_region_encompassing_selected_annotations(annotation_set_t* annotation_set, scene_t* scene) {
	annotation_t* selected_annotation = annotation_set->selected_annotations[0];
	annotation_recalculate_bounds_if_necessary(selected_annotation);
	if (annotation_set->selection_count == 1 && selected_annotation->coordinate_count >= 2) {
		scene->selection_box = bounds2f_to_rect(selected_annotation->bounds);
		scene->crop_bounds = selected_annotation->bounds;
		scene->has_selection_box = true;
	} else if (annotation_set->selection_count > 1) {
		bounds2f bounds = selected_annotation->bounds;
		for (i32 i = 1; i < annotation_set->selection_count; ++i) {
			selected_annotation = annotation_set->selected_annotations[i];
			annotation_recalculate_bounds_if_necessary(selected_annotation);
			bounds = bounds2f_encompassing(bounds, selected_annotation->bounds);
		}
		scene->selection_box = bounds2f_to_rect(bounds);
		scene->crop_bounds = bounds;
		scene->has_selection_box = true;
	}
};

void annotation_draw_coordinate_dot(ImDrawList* draw_list, v2f point, float node_size, rgba_t node_color) {
	draw_list->AddCircleFilled(point, node_size, *(u32*)(&node_color), 12);
}

enum annotation_draw_condition_enum {
	ANNOTATION_DRAW_NEVER = 0,
	ANNOTATION_DRAW_ALWAYS = 1,
	ANNOTATION_DRAW_IF_SELECTED = 2,
	ANNOTATION_DRAW_IF_AT_LEAST_ONE_FEATURE_SET = 3,
};

annotation_draw_condition_enum annotation_draw_fill_area_condition = ANNOTATION_DRAW_IF_AT_LEAST_ONE_FEATURE_SET;

bool annotation_need_draw_fill_area(annotation_t* annotation) {
	if (annotation_highlight_inside_of_polygons && annotation->coordinate_count >= 3 && !annotation->is_open) {
		switch(annotation_draw_fill_area_condition) {
			default:
			case ANNOTATION_DRAW_NEVER: return false; break;
			case ANNOTATION_DRAW_ALWAYS: return true; break;
			case ANNOTATION_DRAW_IF_SELECTED: return annotation->selected; break;
			case ANNOTATION_DRAW_IF_AT_LEAST_ONE_FEATURE_SET: {
				// recalculate nonzero feature count if necessary
				if (!(annotation->valid_flags & ANNOTATION_VALID_NONZERO_FEATURE_COUNT)) {
					u32 nonzero_count = 0;
					for (i32 i = 0; i < MAX_ANNOTATION_FEATURES; ++i) {
						float value = annotation->features[i];
						if (value != 0.0f) {
							++nonzero_count;
						}
					}
					annotation->nonzero_feature_count = nonzero_count;
					annotation->valid_flags |= ANNOTATION_VALID_NONZERO_FEATURE_COUNT;
				}
				return (annotation->nonzero_feature_count > 0);
			} break;
		}
	}
	return false;
}

static void draw_annotation_fill_area(temp_memory_t* temp_memory, app_state_t* app_state, scene_t* scene, v2f camera_min, annotation_t* annotation, rgba_t fill_color) {
	if (!(annotation->valid_flags & ANNOTATION_VALID_TESSELATION)) {
		// Performance: don't tesselate large polygons too often (this is CPU intensive!)
		if (annotation->coordinate_count < 10 || app_state->mouse_mode != MODE_DRAG_ANNOTATION_NODE) {
			//  Invoke the triangulator to triangulate this polygon.
				arrsetlen(annotation->tesselated_trianges, 0);
			if (triangulate_process(annotation->coordinates, annotation->coordinate_count, &annotation->tesselated_trianges)) {
				DUMMY_STATEMENT;
				// success
			} else {
				annotation->is_complex_polygon = true;
			}
			annotation->valid_flags |= ANNOTATION_VALID_TESSELATION;
		}
	}
	if ((annotation->valid_flags | annotation->fallback_valid_flags) & ANNOTATION_VALID_TESSELATION) {
		i32 triangle_count = arrlen(annotation->tesselated_trianges) / 3;
		if (triangle_count > 0) {

			v2f* vertices = (v2f*) arena_push_size(temp_memory->arena, sizeof(v2f) * triangle_count * 3);
			for (i32 i = 0; i < triangle_count * 3; ++i) {
				vertices[i] = world_pos_to_screen_pos(scene, annotation->tesselated_trianges[i]);
			}

			ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
			for (i32 i = 0; i < triangle_count; ++i) {
				v2f* T = vertices + i * 3;
				draw_list->AddTriangleFilled(T[0], T[1], T[2], *(u32*)(&fill_color));
			}
		}
	}
}

void draw_annotations(app_state_t* app_state, scene_t* scene, annotation_set_t* annotation_set, v2f camera_min) {
	if (!scene->enable_annotations) return;

	recount_selected_annotations(app_state, annotation_set);

	bool did_popup = false;

	for (i32 annotation_index = 0; annotation_index < annotation_set->active_annotation_count; ++annotation_index) {
		temp_memory_t temp_memory = begin_temp_memory_on_local_thread();
		annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);

		// Don't draw the annotation if it's out of view
		annotation_recalculate_bounds_if_necessary(annotation);
		bounds2f extruded_camera_bounds = scene->camera_bounds;
		float extrude_amount = 30.0f * scene->zoom.screen_point_width; // prevent pop-in at the edges e.g. due to the added thickness of the annotation outline
		extruded_camera_bounds.min.x -= extrude_amount;
		extruded_camera_bounds.min.y -= extrude_amount;
		extruded_camera_bounds.max.x += extrude_amount;
		extruded_camera_bounds.max.y += extrude_amount;
		if (!are_bounds2f_overlapping(extruded_camera_bounds, annotation->bounds)) {
			continue;
		}

		annotation_group_t* group = annotation_set->stored_groups + annotation->group_id;
//		rgba_t rgba = {50, 50, 0, 255 };
		rgba_t base_color = group->color;
		u8 alpha = (u8)(annotation_opacity * 255.0f);
		base_color.a = alpha;
		float thickness = annotation_normal_line_thickness;
		if (annotation->selected) {
			base_color.r = LERP(0.2f, base_color.r, 255);
			base_color.g = LERP(0.2f, base_color.g, 255);
			base_color.b = LERP(0.2f, base_color.b, 255);
			thickness = annotation_selected_line_thickness;
		}
		u32 annotation_color = *(u32*)(&base_color);

		ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

		// Prevent acute angles in annotations being drawn incorrectly (at least until ImGui bug is fixed):
		// https://github.com/ocornut/imgui/issues/3366
		// https://github.com/ocornut/imgui/pull/2964
		ImDrawListFlags backup_flags = draw_list->Flags;
//		draw_list->Flags &= ~ImDrawListFlags_AntiAliasedLines;

		if (annotation->coordinate_count > 0) {
			v2f* points = (v2f*) arena_push_size(temp_memory.arena, sizeof(v2f) * annotation->coordinate_count);
			for (i32 i = 0; i < annotation->coordinate_count; ++i) {
				points[i] = world_pos_to_screen_pos(scene, annotation->coordinates[i]);
			}

			// Only draw the closing line back to the starting point if needed
			bool closed = !annotation->is_open;

			// Draw the inside of the annotation
			if (annotation_need_draw_fill_area(annotation)) {
				rgba_t fill_color = base_color;
				fill_color.a = (u8)(annotation_highlight_opacity * 255.0f);
				draw_annotation_fill_area(&temp_memory, app_state, scene, camera_min, annotation, fill_color);
			}

			bool need_draw_nodes = annotation->type == ANNOTATION_POINT ||
			                       (annotation->selected && (annotation_show_polygon_nodes_outside_edit_mode ||
			                                                 annotation_set->is_edit_mode || annotation_set->editing_annotation_index == annotation_index));


			rgba_t line_color = base_color;
			if (need_draw_nodes) {
				// make nodes stand out more by making the line transparent
				line_color.a /= 2;
			}

			// Draw the annotation in the background list (behind UI elements), as a thick colored line
			if (annotation->coordinate_count >= 4) {
				gui_draw_polygon_outline(points, annotation->coordinate_count, line_color, closed, thickness);
			} else if (annotation->coordinate_count >= 2) {
				draw_list->AddLine(points[0], points[1], *(u32*)(&line_color), thickness);
				if (annotation->coordinate_count == 3) {
					draw_list->AddLine(points[1], points[2], *(u32*)(&line_color), thickness);
					if (closed) {
						draw_list->AddLine(points[2], points[0], *(u32*)(&line_color), thickness);
					}
				}
			} else if (annotation->coordinate_count == 1) {
				//annotation_draw_coordinate_dot(draw_list, points[0], annotation_node_size * 0.7f, base_color);
			}

			// draw coordinate nodes
			if (need_draw_nodes) {
				bool need_hover = false;
				v2f hovered_node_point = {};
				for (i32 coordinate_index = 0; coordinate_index < annotation->coordinate_count; ++coordinate_index) {
					v2f point = points[coordinate_index];
					if (annotation_set->is_edit_mode && !annotation_set->is_insert_coordinate_mode &&
					    annotation_index == annotation_set->hovered_annotation &&
					    coordinate_index == annotation_set->hovered_coordinate &&
					    annotation_set->hovered_coordinate_pixel_distance < annotation_hover_distance)
					{
						hovered_node_point = point;
						need_hover = true;
					} else {
						annotation_draw_coordinate_dot(draw_list, point, annotation_node_size, base_color);
					}
				}
				if (need_hover) {
//											rgba_t node_rgba = {0, 0, 0, 255};
//					rgba_t node_rgba = {(u8)(group->color.r / 2) , (u8)(group->color.g / 2), (u8)(group->color.b / 2), 255};
					rgba_t hover_color = group->color;
					hover_color.a = alpha;
					draw_list->AddCircleFilled(hovered_node_point, annotation_node_size * 1.4f, *(u32*)(&hover_color), 12);
					if (ImGui::BeginPopupContextVoid()) {
						did_popup = true;
						if (!annotation->selected) {
							select_annotation(annotation_set, annotation);
						}

						if (annotation_set->active_annotation_count > 0) {
							if (ImGui::MenuItem("Allow editing coordinates", "E", &annotation_set->is_edit_mode, annotation_set->active_annotation_count > 0)) {}
							ImGui::Separator();
						}

						if (ImGui::MenuItem("Delete coordinate", "C")) {
							delete_coordinate(annotation_set, annotation_index, annotation_set->hovered_coordinate);
						};
						if (ImGui::MenuItem("Insert coordinate", "Shift", &annotation_set->force_insert_mode)) {
							annotation_set->is_insert_coordinate_mode = true;
							annotation_set->is_split_mode = false;
						};
						if (ImGui::MenuItem("Split annotation here")) {
							annotation_set->selected_coordinate_index = annotation_set->hovered_coordinate;
							annotation_set->selected_coordinate_annotation_index = annotation_set->hovered_annotation;
							annotation_set->is_split_mode = true;
							annotation_set->is_insert_coordinate_mode = false;
							annotation_set->force_insert_mode = false;
						}
						ImGui::Separator();
						if (gui_draw_selected_annotation_submenu_section(app_state, scene, annotation_set)) {
							ImGui::Separator();
						}

						gui_draw_insert_annotation_submenu(app_state);

						ImGui::EndPopup();
					}
				}

				if (annotation_set->is_edit_mode && (annotation_set->is_insert_coordinate_mode)) {
					v2f projected_point = {};
					float distance = FLT_MAX;
					i32 insert_before_index = project_point_onto_annotation(annotation_set, annotation,
					                                                        app_state->scene.mouse, NULL,
					                                                        &projected_point, &distance);
					if (insert_before_index >= 0) {
						float transformed_distance = distance / scene->zoom.screen_point_width;
						if (transformed_distance < annotation_insert_hover_distance) {
							// Draw a partially transparent, slightly larger ('hovering' size) node circle at the projected point
							v2f transformed_pos = world_pos_to_screen_pos(scene, projected_point);
							rgba_t hover_color = group->color;
							hover_color.a = alpha / 2;
							draw_list->AddCircleFilled(transformed_pos, annotation_node_size * 1.4f, *(u32*)(&hover_color), 12);
						}
					}
				}

				// Draw the 'preview' line displayed just before you split an annotation
				if (annotation_set->is_edit_mode && annotation_set->is_split_mode && annotation_index == annotation_set->selected_coordinate_annotation_index) {
					if (annotation_set->selected_coordinate_index >= 0 && annotation_set->selected_coordinate_index < annotation->coordinate_count) {
						v2f split_coordinate = annotation->coordinates[annotation_set->selected_coordinate_index];
						v2f transformed_pos = world_pos_to_screen_pos(scene, split_coordinate);
						v2f split_line_points[2] = {};
						split_line_points[0] = transformed_pos;
						split_line_points[1] = world_pos_to_screen_pos(scene, app_state->scene.mouse);
						draw_list->AddLine(split_line_points[0], split_line_points[1], annotation_color, thickness);
					} else {
						if (DO_DEBUG) {
							console_print_error("Error: tried to draw line for annotation split mode, but the selected coordinate (%d) is invalid for this annotation\n", annotation_set->selected_coordinate_index);
						}
					}
				}

			}

			if (app_state->mouse_tool == TOOL_CREATE_FREEFORM && annotation_index == annotation_set->editing_annotation_index) {
				if (annotation->is_open && annotation->coordinate_count > 0) {
					v2f last = annotation->coordinates[annotation->coordinate_count-1];
					v2f line_points[2] = {};
					line_points[0] = world_pos_to_screen_pos(scene, last);
					line_points[1] = world_pos_to_screen_pos(scene, app_state->scene.mouse);
					draw_list->AddLine(line_points[0], line_points[1], annotation_color, thickness);
				}
			}

		} else {
			// Annotation does NOT have coordinates
			if (annotation->type == ANNOTATION_ELLIPSE) {
				v2f p0 = world_pos_to_screen_pos(scene, annotation->p0);
				v2f p1 = world_pos_to_screen_pos(scene, annotation->p1);
				v2f center = v2f_average(p0, p1);
				v2f v = v2f_subtract(p1, p0);
				float len = v2f_length(v);
				float angle = atan2f(-v.y, v.x);
				float radius_x = cosf(angle) * len;
				float radius_y = sinf(angle) * len;
//				float radius_average = fabsf(radius_x) + fabsf(radius_y) * 0.5f;
//				draw_list->AddCircle(center, radius_average, IM_COL32(255, 255, 0, 255), 0, 2.0f);

				if (len > 50.0f) {
					DUMMY_STATEMENT;
				}

				const i32 segment_count = 48;
				v2f p_ellipse[segment_count] = {};
				float theta = 0;
				float theta_step = (IM_PI * 2.0f) / segment_count;
				for (i32 i = 0; i < segment_count; ++i) {
					p_ellipse[i].x = center.x + radius_x * cosf(theta);
					p_ellipse[i].y = center.y + radius_y * sinf(theta);
					theta += theta_step;
				}
				draw_list->AddPolyline((const ImVec2*)(p_ellipse), segment_count, IM_COL32(255, 255, 0, 255), ImDrawFlags_Closed, 2.0f);
			}
		}

		draw_list->Flags = backup_flags;
		release_temp_memory(&temp_memory);
	}

	if (!did_popup) {
		if (ImGui::BeginPopupContextVoid()) {
			did_popup = true;
			if (annotation_set->active_annotation_count > 0) {
				if (ImGui::MenuItem("Allow editing coordinates", "E", &annotation_set->is_edit_mode)) {}
				ImGui::Separator();
			}

			if (gui_draw_selected_annotation_submenu_section(app_state, scene, annotation_set)) {
				ImGui::Separator();
			}

			gui_draw_insert_annotation_submenu(app_state);

			ImGui::EndPopup();
		}
	}
}

void center_scene_on_annotation(scene_t* scene, annotation_t* annotation) {
	bounds2f bounds = bounds_for_annotation(annotation);
	v2f center = {(bounds.max.x + bounds.min.x)*0.5f, (bounds.max.y + bounds.min.y)*0.5f};
	scene_update_camera_pos(scene, center);
}

void draw_annotations_window(app_state_t* app_state, input_t* input) {

	scene_t* scene = &app_state->scene;
	annotation_set_t* annotation_set = &app_state->scene.annotation_set;

	// NOTE: We need to allocate one extra (placeholder) preview, to prevent going out of bounds when the user clicks
	// the button to create a new group/feature.
	const char** group_item_previews = (const char**) alloca((annotation_set->active_group_count + 1) * sizeof(char*));
	for (i32 i = 0; i < annotation_set->active_group_count; ++i) {
		annotation_group_t* group = get_active_annotation_group(annotation_set, i);
		group_item_previews[i] = group->name;
	}
	group_item_previews[annotation_set->active_group_count] = "";

	const char** feature_item_previews = (const char**) alloca((annotation_set->active_feature_count + 1) * sizeof(char*));
	for (i32 i = 0; i < annotation_set->active_feature_count; ++i) {
		annotation_feature_t* feature = get_active_annotation_feature(annotation_set, i);
		feature_item_previews[i] = feature->name;
	}
	feature_item_previews[annotation_set->active_feature_count] = "";

	// find group corresponding to the currently selected annotations
	i32 annotation_group_index = -1;
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* annotation = get_active_annotation(annotation_set, i);
		if (annotation->selected) {
			if (annotation_group_index == -1) {
				annotation_group_index = annotation->group_id;
			} else if (annotation_group_index != annotation->group_id) {
				annotation_group_index = -2; // multiple groups selected
			}

		}
	}
	bool nothing_selected = (annotation_group_index == -1);
	bool multiple_groups_selected = (annotation_group_index == -2);


	// Detect hotkey presses for group assignment
	bool* hotkey_pressed = (bool*) alloca(annotation_set->active_group_count * sizeof(bool));
	memset(hotkey_pressed, 0, annotation_set->active_group_count * sizeof(bool));

	if (!gui_want_capture_keyboard) {
		for (i32 i = 0; i < ATMOST(9, annotation_set->active_group_count); ++i) {
			if (was_key_pressed(input, KEY_1+i)) {
				hotkey_pressed[i] = true;
			}
		}
		if (annotation_set->active_group_count >= 10 && was_key_pressed(input, KEY_0)) {
			hotkey_pressed[9] = true;
		}
	}

	const char* group_preview_string = "";
	if (annotation_group_index >= 0 && annotation_group_index < annotation_set->active_group_count) {
		group_preview_string = group_item_previews[annotation_group_index];
	} else if (multiple_groups_selected) {
		group_preview_string = "(multiple)"; // if multiple annotations with different groups are selected
	} else if (nothing_selected) {
		group_preview_string = "(nothing selected)";
	}

	if (show_annotations_window) {

		ImGui::SetNextWindowPos(ImVec2(830,43), ImGuiCond_FirstUseEver, ImVec2());
		ImGui::SetNextWindowSize(ImVec2(525,673), ImGuiCond_FirstUseEver);

		ImGui::Begin("Annotations", &show_annotations_window, 0);

        const char* annotation_filename = "";
		if (app_state->export_as_coco) {
            annotation_filename = annotation_set->coco_filename;
		} else if (annotation_set->export_as_asap_xml) {
            annotation_filename = annotation_set->asap_xml_filename;
		}
        if (annotation_filename[0] != '\0') {
            ImGui::TextWrapped("Annotation filename: %s\n", annotation_filename);
        } else {
            ImGui::TextUnformatted("Annotation filename: (none)\n");
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
        if (ImGui::SmallButton("Annotation directory: ")) {
            open_file_dialog(app_state, OPEN_FILE_DIALOG_CHOOSE_DIRECTORY, 0);
        }
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(1);
        ImGui::SameLine();
        ImGui::TextWrapped("%s\n", get_annotation_directory(app_state));

		ImGui::Text("Number of annotations active: %d\n", annotation_set->active_annotation_count);
		ImGui::Spacing();

//		if (ImGui::CollapsingHeader("Annotation"))
		{
			if (!scene->enable_annotations) {
				ImGui::BeginDisabled();
			}

			// GUI for selecting an annotation using a slider input
			static i32 annotation_to_select = -1;

			// Update which annotation is currently displayed to be selected
			if (annotation_set->selection_count == 1) {
				for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
					annotation_t* a = get_active_annotation(annotation_set, i);
					if (a->selected) {
						annotation_to_select = i;
						break;
					}
				}
			}
			annotation_to_select = CLAMP(annotation_to_select, -1, annotation_set->active_annotation_count - 1);
			if (ImGui::DragInt("Select annotation", &annotation_to_select, 0.25f, -1, annotation_set->active_annotation_count - 1)) {
				// Deselect all annotations except for this one
				for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
					annotation_t* a = get_active_annotation(annotation_set, i);
					if (i == annotation_to_select) {
						a->selected = true;
						// Center the screen on the new annotation
						center_scene_on_annotation(&app_state->scene, a);

					} else {
						a->selected = false;
					}
				}
			}

			u32 selectable_flags = 0;
			if (nothing_selected) {
				ImGui::BeginDisabled();
			}

			if (annotation_set->selection_count == 1) {
				annotation_t* selected_annotation = annotation_set->selected_annotations[0];
				if (selected_annotation) {
					if (ImGui::InputText("Name", selected_annotation->name, sizeof(selected_annotation->name))) {
						notify_annotation_set_modified(annotation_set);
					}
				}
			} else {
				ImGui::BeginDisabled();
				char temp[256];
				temp[0] = '\0';
				ImGui::InputText("Name", temp, sizeof(temp), 0);
				ImGui::EndDisabled();
			}

			static const char* annotation_types[] = {"Unknown type", "Rectangle", "Polygon", "Point", "Line", "Spline", "Ellipse", "Text" };

			// Figure out which types have been selected
			i32 annotation_type_index = -1;
			for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
				annotation_t* annotation = get_active_annotation(annotation_set, i);
				if (annotation->selected) {
					if (annotation_type_index == -1) {
						annotation_type_index = annotation->type;
					} else if (annotation_type_index != annotation->type) {
						annotation_type_index = -2; // multiple types selected
					}
				}
			}
			bool no_types_selected = (annotation_type_index == -1);
			bool multiple_types_selected = (annotation_type_index == -2);

			const char* type_preview_string = "";
			if (annotation_type_index >= 0 && annotation_type_index < annotation_set->active_group_count) {
				type_preview_string = annotation_types[annotation_type_index];
			} else if (multiple_types_selected) {
				type_preview_string = "(multiple)"; // if multiple annotations with different groups are selected
			} else if (no_types_selected) {
				type_preview_string = "(nothing selected)";
			}

#if DO_DEBUG
			if (ImGui::BeginCombo("Type##annotation_debug_select_type", type_preview_string, ImGuiComboFlags_HeightLargest)) {
				for (i32 type_index = 0; type_index < COUNT(annotation_types); ++type_index) {

					if (ImGui::Selectable(annotation_types[type_index], (annotation_type_index == type_index), 0, ImVec2())) {
						set_type_for_selected_annotations(annotation_set, type_index);
					}
				}
				ImGui::EndCombo();
			}
#else
			ImGui::Text("Type: %s", type_preview_string);
#endif

			if (ImGui::BeginCombo("Group##annotation_window_assign_group", group_preview_string, ImGuiComboFlags_HeightLargest)) {
				for (i32 group_index = 0; group_index < annotation_set->stored_group_count; ++group_index) {
					annotation_group_t* group = annotation_set->stored_groups + group_index;

					if (ImGui::Selectable(group_item_previews[group_index], (annotation_group_index == group_index), 0, ImVec2())
					|| ((!nothing_selected) && hotkey_pressed[group_index])) {
						set_group_for_selected_annotations(annotation_set, group_index);
					}
				}
				ImGui::EndCombo();
			}

			if (nothing_selected) {
				ImGui::EndDisabled();
			}

			if (!scene->enable_annotations) {
				ImGui::EndDisabled();
			}

			if (ImGui::Button("Assign group or feature...")) {
				show_annotation_group_assignment_window = true;
			}

			ImGui::NewLine();

		}


		if (ImGui::CollapsingHeader("Dataset info")) {
			if (ImGui::InputText("Description", annotation_set->coco.info.description, COCO_MAX_FIELD)) {
				notify_annotation_set_modified(annotation_set);
			}
			if (ImGui::InputText("URL", annotation_set->coco.info.url, COCO_MAX_FIELD)) {
				notify_annotation_set_modified(annotation_set);
			}
			if (ImGui::InputText("Version", annotation_set->coco.info.version, COCO_MAX_FIELD)) {
				notify_annotation_set_modified(annotation_set);
			}
			if (ImGui::InputInt("Year", &annotation_set->coco.info.year, 1, 100)) {
				notify_annotation_set_modified(annotation_set);
			}
			if (ImGui::InputText("Contributor", annotation_set->coco.info.contributor, COCO_MAX_FIELD)) {
				notify_annotation_set_modified(annotation_set);
			}
			if (ImGui::InputText("Date created", annotation_set->coco.info.date_created, COCO_MAX_FIELD)) {
				notify_annotation_set_modified(annotation_set);
			}
			// TODO: Image, license etc.
		}

		// Interface for viewing/editing annotation groups
		if (ImGui::CollapsingHeader("Edit groups"))
		{
			static i32 edit_group_index = 0;
			const char* edit_group_preview = "";
            bool edit_group_is_valid = false;

			if (edit_group_index >= 0 && edit_group_index < annotation_set->active_group_count) {
				edit_group_preview = group_item_previews[edit_group_index];
                edit_group_is_valid = true;
			} else {
                edit_group_index = 0;
			}

			bool disable_interface = annotation_set->active_group_count <= 0;

			if (disable_interface) {
				ImGui::BeginDisabled();
			}

			ImGui::Text("Number of groups: %d\n", annotation_set->active_group_count);
			if (ImGui::BeginCombo("Select group##annotation_edit_groups_select_group", "", ImGuiComboFlags_HeightLargest)) {
				for (i32 group_index = 0; group_index < annotation_set->active_group_count; ++group_index) {
					annotation_group_t* group = get_active_annotation_group(annotation_set, group_index);

					if (ImGui::Selectable(group_item_previews[group_index], (edit_group_index == group_index))) {
						edit_group_index = group_index;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::Spacing();

			annotation_group_t* selected_group = NULL;
			if (edit_group_is_valid) {
				selected_group = get_active_annotation_group(annotation_set, edit_group_index);
			}
			const char* group_name = selected_group ? selected_group->name : "";

			if (!disable_interface && selected_group == NULL) {
				disable_interface = true;
				ImGui::BeginDisabled();
			}

			// Text field to display the group name, allowing for renaming.
			{
				static char dummy_buf[64] = "";
				char* group_name_buf = dummy_buf;
				ImGuiInputTextFlags flags;
				if (selected_group) {
					group_name_buf = selected_group->name;
					flags = 0;
				} else {
					group_name_buf = dummy_buf;
					flags = ImGuiInputTextFlags_ReadOnly;
				}
				if (ImGui::InputText("Group name", group_name_buf, 64)) {
					notify_annotation_set_modified(annotation_set);
				}
			}

			// Color picker for editing the group color.
			ImGuiColorEditFlags flags = 0;
			float color[3] = {};
			if (edit_group_is_valid) {
				annotation_group_t* group = annotation_set->stored_groups + edit_group_index;
				rgba_t rgba = group->color;
				color[0] = BYTE_TO_FLOAT(rgba.r);
				color[1] = BYTE_TO_FLOAT(rgba.g);
				color[2] = BYTE_TO_FLOAT(rgba.b);
				if (ImGui::ColorEdit3("Group color", (float*) color, flags)) {
					rgba.r = FLOAT_TO_BYTE(color[0]);
					rgba.g = FLOAT_TO_BYTE(color[1]);
					rgba.b = FLOAT_TO_BYTE(color[2]);
					group->color = rgba;
					notify_annotation_set_modified(annotation_set);
				}
			} else {
				flags = ImGuiColorEditFlags_NoPicker;
				ImGui::ColorEdit3("Group color", (float*) color, flags);
			}

			if (ImGui::Button("Delete group")) {
				//annotation_group_delete(annotation_set, edit_group_index);
				if (selected_group) {
					selected_group->deleted = true;
				}
			}

			if (disable_interface) {
				ImGui::EndDisabled();
			}

			ImGui::SameLine();
			if (ImGui::Button("Add group")) {
				char new_group_name[64];
				snprintf(new_group_name, 64, "Group %d", annotation_set->stored_group_count);
				edit_group_index = add_annotation_group(annotation_set, new_group_name);
				notify_annotation_set_modified(annotation_set);
			}

			ImGui::NewLine();


		}

		if (ImGui::CollapsingHeader("Edit features"))
		{
			static i32 edit_feature_index = -1;
			const char* edit_feature_preview = "";

			if (edit_feature_index >= 0 && edit_feature_index < annotation_set->active_feature_count) {
				edit_feature_preview = feature_item_previews[edit_feature_index];
			} else {
				edit_feature_index = -1;
			}

			bool disable_interface = annotation_set->active_feature_count <= 0;

			if (disable_interface) {
				ImGui::BeginDisabled();
			}

			ImGui::Text("Number of features: %d\n", annotation_set->active_feature_count);
			if (ImGui::BeginCombo("Select feature", "", ImGuiComboFlags_HeightLargest)) {
				for (i32 feature_index = 0; feature_index < annotation_set->active_feature_count; ++feature_index) {
					annotation_feature_t* feature = annotation_set->stored_features + annotation_set->active_feature_indices[feature_index];

					if (ImGui::Selectable(feature_item_previews[feature_index], (edit_feature_index == feature_index), 0)) {
						edit_feature_index = feature_index;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::Spacing();

			annotation_feature_t* selected_feature = NULL;
			if (edit_feature_index >= 0) {
				selected_feature = annotation_set->stored_features + annotation_set->active_feature_indices[edit_feature_index];
			}
			const char* feature_name = selected_feature ? selected_feature->name : "";

			if (!disable_interface && selected_feature == NULL) {
				disable_interface = true;
				ImGui::BeginDisabled();
			}

			// Text field to display the feature name, allowing for renaming.
			{
				static char dummy_buf[64] = "";
				char* feature_name_buf = dummy_buf;
				ImGuiInputTextFlags flags;
				if (selected_feature) {
					feature_name_buf = selected_feature->name;
					flags = 0;
				} else {
					feature_name_buf = dummy_buf;
					flags = ImGuiInputTextFlags_ReadOnly;
				}
				if (ImGui::InputText("Feature name", feature_name_buf, 64)) {
					notify_annotation_set_modified(annotation_set);
				}
			}

			bool restrict_to_group = selected_feature ? selected_feature->restrict_to_group : false;
			if (ImGui::Checkbox("Restrict to group", &restrict_to_group)) {
				if (selected_feature) selected_feature->restrict_to_group = restrict_to_group;
				notify_annotation_set_modified(annotation_set);
			}

			if (!restrict_to_group) {
				ImGui::BeginDisabled();
			}
			annotation_group_t* feature_group = NULL;
			const char* feature_group_preview = "";
			if (selected_feature) {
				if (selected_feature->group_id >= 0 && selected_feature->group_id < annotation_set->stored_group_count) {
					feature_group = annotation_set->stored_groups + selected_feature->group_id;
					feature_group_preview = feature_group->name;
				}
			}
			if (ImGui::BeginCombo("Group##feature_restrict_to_group", feature_group_preview, ImGuiComboFlags_HeightLargest)) {
				if (edit_feature_index >= 0) {
					annotation_feature_t* feature = annotation_set->stored_features + annotation_set->active_feature_indices[edit_feature_index];
					for (i32 group_index = 0; group_index < annotation_set->stored_group_count; ++group_index) {
						annotation_group_t* group = annotation_set->stored_groups + group_index;

						if (ImGui::Selectable(group_item_previews[group_index], (feature->group_id == group_index), 0, ImVec2())) {
							feature->group_id = group_index;
							notify_annotation_set_modified(annotation_set);
						}
					}
				}

				ImGui::EndCombo();
			}
			if (!restrict_to_group) {
				ImGui::EndDisabled();
			}

#if 0
			// Color picker for editing the feature color.
			ImGuiColorEditFlags flags = 0;
			float color[3] = {};
			if (edit_feature_index >= 0) {
				annotation_feature_t* feature = annotation_set->stored_features + edit_feature_index;
				rgba_t rgba = feature->color;
				color[0] = BYTE_TO_FLOAT(rgba.r);
				color[1] = BYTE_TO_FLOAT(rgba.g);
				color[2] = BYTE_TO_FLOAT(rgba.b);
				if (ImGui::ColorEdit3("Feature color", (float*) color, flags)) {
					rgba.r = FLOAT_TO_BYTE(color[0]);
					rgba.g = FLOAT_TO_BYTE(color[1]);
					rgba.b = FLOAT_TO_BYTE(color[2]);
					feature->color = rgba;
					annotations_modified(annotation_set);
				}
			} else {
				flags = ImGuiColorEditFlags_NoPicker;
				ImGui::ColorEdit3("Feature color", (float*) color, flags);
			}
#endif

			if (ImGui::Button("Delete feature")) {
				if (edit_feature_index >= 0) {
					annotation_feature_delete(annotation_set, edit_feature_index);
				}
				edit_feature_index = -1;
			}

			if (disable_interface) {
				ImGui::EndDisabled();
			}

			ImGui::SameLine();
			if (ImGui::Button("Add feature")) {
				char new_feature_name[64];
				snprintf(new_feature_name, 64, "Feature %d", annotation_set->stored_feature_count);
				edit_feature_index = add_annotation_feature(annotation_set, new_feature_name);
				notify_annotation_set_modified(annotation_set);
			}


			ImGui::NewLine();


		}

		if (ImGui::CollapsingHeader("Options"))
		{
			ImGui::Checkbox("Show annotations (press H to toggle)", &scene->enable_annotations);
			ImGui::SliderFloat("Annotation opacity", &annotation_opacity, 0.0f, 1.0f, "%.3f");

			ImGui::SliderFloat("Line thickness (normal)", &annotation_normal_line_thickness, 0.0f, 10.0f, "%.1f px");
			ImGui::SliderFloat("Line thickness (selected)", &annotation_selected_line_thickness, 0.0f, 10.0f, "%.1f px");

			ImGui::NewLine();

			ImGui::Checkbox("Enable highlighting the inside of annotations", &annotation_highlight_inside_of_polygons);

			if (!annotation_highlight_inside_of_polygons) {
				ImGui::BeginDisabled();
			}

			static const char* highlight_options[] = {"Never", "Always", "If selected", "If at least one feature is set"};
			u32 current_highlight_condition = annotation_draw_fill_area_condition;
			const char* current_highlight_string = "";
			if (current_highlight_condition < COUNT(highlight_options)) {
				current_highlight_string = highlight_options[current_highlight_condition];
			}
			if (ImGui::BeginCombo("Highlight condition", current_highlight_string, ImGuiComboFlags_HeightLargest)) {
				for (i32 i = 1; i < COUNT(highlight_options); ++i) {
					if (ImGui::Selectable(highlight_options[i], current_highlight_condition == i, 0, ImVec2())) {
						annotation_draw_fill_area_condition = (annotation_draw_condition_enum)i;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SliderFloat("Highlight opacity", &annotation_highlight_opacity, 0.0f, 1.0f, "%.2f");

			if (!annotation_highlight_inside_of_polygons) {
				ImGui::EndDisabled();
			}



			ImGui::NewLine();

			//			ImGui::Checkbox("Show polygon nodes", &annotation_show_polygon_nodes_outside_edit_mode);
			ImGui::Checkbox("Allow editing annotation coordinates (press E to toggle)", &annotation_set->is_edit_mode);
			ImGui::SliderFloat("Coordinate node size", &annotation_node_size, 0.0f, 20.0f, "%.1f px");

			ImGui::NewLine();

			ImGui::SliderFloat("Freeform node interval", &annotation_freeform_insert_interval_distance, 1.0f, 100.0f, "%.0f px");

			ImGui::NewLine();
			ImGui::Checkbox("Save in both XML and JSON formats", &app_state->export_as_coco);
		}


		ImGui::End();
	}


	if (show_annotation_group_assignment_window) {

		ImGui::SetNextWindowPos(ImVec2(1288,42), ImGuiCond_FirstUseEver, ImVec2());
		ImGui::SetNextWindowSize(ImVec2(285,572), ImGuiCond_FirstUseEver);

		ImGui::Begin("Assign", &show_annotation_group_assignment_window);

		ImGui::TextUnformatted(group_preview_string);

		ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
		if (ImGui::BeginTabBar("Feature assignment tab bar", tab_bar_flags))
		{
			if (ImGui::BeginTabItem("Groups"))
			{
				if (nothing_selected) {
					ImGui::BeginDisabled();
				}

				for (i32 group_index = 0; group_index < annotation_set->active_group_count; ++group_index) {
					annotation_group_t* group = annotation_set->stored_groups + group_index;

					u32 rgba_u32 = *(u32*) &group->color;
					ImVec4 color = ImColor(rgba_u32);

					static int e = 0;
					ImGui::PushID(group_index);
					color.w = 1.0f;
					ImGui::PushStyleColor(ImGuiCol_CheckMark, color);
//		            color.w = 0.6f;
//		            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color);
//		            color.w = 0.7f;
//		            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, color);
					bool pressed_hotkey = false;
					if (!nothing_selected) {
						if (group_index >= 0 && group_index < 10) {
							pressed_hotkey = hotkey_pressed[group_index];
						}
					}

					if (ImGui::Selectable("", (annotation_group_index == group_index), 0, ImVec2(0,ImGui::GetFrameHeight()))
					    || pressed_hotkey) {
						set_group_for_selected_annotations(annotation_set, group_index);
					}

					ImGui::SameLine(0); ImGui::RadioButton(group_item_previews[group_index], &annotation_group_index, group_index);
					if (group_index <= 9) {
						ImGui::SameLine(ImGui::GetWindowWidth()-40.0f);
						if (group_index <= 8) {
							ImGui::Text("[%d]", group_index+1);
						} else if (group_index == 9) {
							ImGui::TextUnformatted("[0]");
						}

					}
					ImGui::PopStyleColor(1);
					ImGui::PopID();

				}

				ImGui::Separator();
				ImGui::Checkbox("Auto-assign last group", &auto_assign_last_group);

				if (nothing_selected) {
					ImGui::EndDisabled();
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Features")) {
				if (nothing_selected) {
					ImGui::BeginDisabled();
				}

				static i32 selectable_feature_ids[MAX_ANNOTATION_FEATURES];
				static float selectable_feature_values[MAX_ANNOTATION_FEATURES];
				static bool selectable_feature_values_are_mixed[MAX_ANNOTATION_FEATURES];
				memset(selectable_feature_ids, 0, sizeof(selectable_feature_ids));
				memset(selectable_feature_values, 0, sizeof(selectable_feature_values));
				memset(selectable_feature_values_are_mixed, 0, sizeof(selectable_feature_values_are_mixed));
				i32 selectable_feature_count = 0;

				if (!nothing_selected) {
					for (i32 feature_index = 0; feature_index < annotation_set->active_feature_count; ++feature_index) {
						annotation_feature_t* feature = annotation_set->stored_features + annotation_set->active_feature_indices[feature_index];
						bool is_feature_allowed = true;
						if (feature->restrict_to_group) {
							for (i32 i = 0; i < annotation_set->selection_count; ++i) {
								annotation_t* selected_annotation = annotation_set->selected_annotations[i];
								is_feature_allowed = (selected_annotation->group_id == feature->group_id);
								if (!is_feature_allowed) break;
							}
						}
						if (is_feature_allowed) {
							i32 selectable_index = selectable_feature_count++;
							selectable_feature_ids[selectable_index] = feature->id;

							// Get the current state of the feature values for the selected annotations
							// NOTE: The values may be mixed if multiple annotations are selected!
							ASSERT(annotation_set->selection_count >= 1);
							annotation_t* first_selected = annotation_set->selected_annotations[0];
							float first_value = first_selected->features[feature->id];
							selectable_feature_values[selectable_index] = first_value;
							bool mixed = false;
							for (i32 i = 1; i < annotation_set->selection_count; ++i) {
								annotation_t* selected = annotation_set->selected_annotations[i];
								float value = selected->features[feature->id];
								if (value != first_value) {
									mixed = true;
									break;
								}
							}
							if (mixed) {
								selectable_feature_values_are_mixed[selectable_index] = true;
								ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
							}

							bool checked = (first_value != 0.0f);

							bool pressed_hotkey = false;
							if (selectable_index >= 0 && selectable_index < 9) {
								pressed_hotkey = was_key_pressed(input, KEY_1 + selectable_index);
							} else if (selectable_index == 9) {
								pressed_hotkey = was_key_pressed(input, KEY_0);
							}
							if (pressed_hotkey) {
								checked = !checked;
							}

							if (ImGui::Checkbox(feature->name, &checked) || pressed_hotkey) {
								// Set value for all selected annotations
								for (i32 i = 0; i < annotation_set->selection_count; ++i) {
									annotation_t* selected = annotation_set->selected_annotations[i];
									float new_value = checked ? 1.0f : 0.0f;
									selected->features[feature->id] = new_value;
									annotation_invalidate_derived_calculations_from_features(selected);
								}
								notify_annotation_set_modified(annotation_set);
							}

							if (selectable_index <= 9) {
								ImGui::SameLine(ImGui::GetWindowWidth()-40.0f);
								if (selectable_index <= 8) {
									ImGui::Text("[%d]", selectable_index+1);
								} else if (selectable_index == 9) {
									ImGui::TextUnformatted("[0]");
								}

							}

							if (mixed) {
								ImGui::PopItemFlag();
							}


						}
					}
				}

				if (selectable_feature_count == 0) {
					ImGui::BeginDisabled();
					ImGui::TextUnformatted("(no features)\n");
					ImGui::EndDisabled();
				}

				if (nothing_selected) {
					ImGui::EndDisabled();
				}
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::End();
	}
}

void annotation_modal_dialog(app_state_t* app_state, annotation_set_t* annotation_set) {
	if (show_delete_annotation_prompt) {
		ImGui::OpenPopup("Delete annotation?");
		show_delete_annotation_prompt = false;
	}
	gui_make_next_window_appear_in_center_of_screen();
	if (ImGui::BeginPopupModal("Delete annotation?", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("The annotation will be deleted.\nThis operation cannot be undone.\n\n");
		ImGui::Separator();

		//static int unused_i = 0;
		//ImGui::Combo("Combo", &unused_i, "Delete\0Delete harder\0");

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::Checkbox("Don't ask me next time", &dont_ask_to_delete_annotations);
		ImGui::PopStyleVar();

		if (ImGui::Button("OK", ImVec2(120, 0)) || was_key_pressed(app_state->input, KEY_Return)) {
			delete_selected_annotations(app_state, annotation_set);
			show_delete_annotation_prompt = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			show_delete_annotation_prompt = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

#include "font_definitions.h"
// https://fontawesome.com/v4.7/cheatsheet/

void draw_annotation_palette_window() {
	if (show_annotation_palette_window) {


		ImGui::SetNextWindowPos(ImVec2(1288,42), ImGuiCond_FirstUseEver, ImVec2());
		ImGui::SetNextWindowSize(ImVec2(285,572), ImGuiCond_FirstUseEver);

		ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
		if (ImGui::Begin("##annotation_palette_window", &show_annotation_palette_window, window_flags)) {

			ImGui::PushFont(global_icon_font);
			if (ImGui::Button(ICON_FA_HAND_PAPER_O "##palette_window")) {}
			if (ImGui::Button(ICON_FA_ARROWS_H "##palette_window")) {}
			if (ImGui::Button(ICON_FA_CIRCLE_O "##palette_window")) {}
			ImGui::PopFont();

			if (ImGui::Button("Edit (E)##palette_window")) {}
			if (ImGui::Button("Add line (M)##palette_window")) {}
			if (ImGui::Button("Add arrow (A)##palette_window")) {}
			if (ImGui::Button("Add text (W)##palette_window")) {}
			if (ImGui::Button("Add ellipse##palette_window")) {}
			if (ImGui::Button("Add circle##palette_window")) {}
			if (ImGui::Button("Add rectangle##palette_window")) {}
			if (ImGui::Button("Add poly (F)##palette_window")) {}
			// Closed/open poly
			if (ImGui::Button("Add notes##palette_window")) {}
			if (ImGui::Button("Classify##palette_window")) {}

			if (ImGui::Button("Toggle grid##palette_window")) {}
			if (ImGui::Button("Toggle scale bar##palette_window")) {}
			if (ImGui::Button("Toggle overview##palette_window")) {}

			ImGui::End();
		}


	}
}

annotation_t duplicate_annotation(annotation_t* annotation) {
	annotation_t result = *annotation;

	// Transfer coordinates
	result.coordinates = NULL;
	arrsetlen(result.coordinates, annotation->coordinate_count);
	memcpy(result.coordinates, annotation->coordinates, annotation->coordinate_count * sizeof(v2f));

	// Invalidate derived calculations
	result.valid_flags = 0;
	result.fallback_valid_flags = 0;
	result.tesselated_trianges = NULL;

	return result;
}

void destroy_annotation(annotation_t* annotation) {
	if (annotation) {
		arrfree(annotation->coordinates);
		arrfree(annotation->tesselated_trianges);
	}
}

void destroy_annotation_set(annotation_set_t* annotation_set) {
	// destroy old state
	for (i32 i = 0; i < annotation_set->stored_annotation_count; ++i) {
		destroy_annotation(annotation_set->stored_annotations + i);
	}
	arrfree(annotation_set->stored_annotations);
	arrfree(annotation_set->active_annotation_indices);
	arrfree(annotation_set->stored_groups);
	arrfree(annotation_set->active_group_indices);
	arrfree(annotation_set->stored_features);
	arrfree(annotation_set->active_feature_indices);
	if (annotation_set->coco.is_valid) coco_destroy(&annotation_set->coco);

}

void unload_and_reinit_annotations(annotation_set_t* annotation_set) {
	destroy_annotation_set(annotation_set);
	memset(annotation_set, 0, sizeof(*annotation_set));

	// initialize new state
	annotation_set->mpp = V2F(1.0f, 1.0f); // default value shouldn't be zero (has danger of divide-by-zero errors)
	annotation_set->editing_annotation_index = -1;
	annotation_set->selected_coordinate_annotation_index = -1;

	// TODO: check is this still needed?
	// reserve annotation group 0 for the "None" category
	i32 group_index = add_annotation_group(annotation_set, "None");
    annotation_group_t* group = get_active_annotation_group(annotation_set, group_index);
	annotation_set->export_as_asap_xml = true;

	annotation_set->coco = coco_create_empty();
}

void geojson_print_feature(FILE* fp, annotation_set_t* annotation_set, annotation_t* annotation) {
	const char* geometry_type;
	switch(annotation->type) {
		default:
		case ANNOTATION_RECTANGLE:
		case ANNOTATION_POLYGON:
		case ANNOTATION_SPLINE:
			geometry_type = "Polygon"; break;
		case ANNOTATION_POINT:
			geometry_type = "Point"; break;
		case ANNOTATION_LINE:
			geometry_type = "LineString"; break;
	}
	fprintf(fp, "  {\n"
			    "    \"type\": \"Feature\",\n"
	            "    \"geometry\": {\n"
			    "      \"type\": \"%s\","
	            "      \"coordinates\": ", geometry_type);
	if (annotation->coordinate_count > 0) {
		for (i32 coordinate_index = 0; coordinate_index < annotation->coordinate_count; ++coordinate_index) {
			if (coordinate_index != 0) {
				fprintf(fp, ", ");
			}
			v2f* coordinate = annotation->coordinates + coordinate_index;
			fprintf(fp, "[%g, %g]", coordinate->x / annotation_set->mpp.x, coordinate->y / annotation_set->mpp.y);
		}
	}
	fprintf(fp, "\n"
			     "    }");
	if (annotation->has_properties) {
		fprintf(fp,     ",\n"
		            "    \"properties\": {\n"
		            "      \"group\": ");
	}
	fprintf(fp, "  }\n"
	            "}");

}

void save_geojson_annotations(annotation_set_t* annotation_set, const char* filename_out) {
	ASSERT(annotation_set);
	FILE* fp = fopen(filename_out, "wb");
	if (fp) {
		i32 indent = 0;
		fprintf(fp, "{\n"
		            "  \"type\": \"FeatureCollection\",\n"
			        "  \"features\": [\n");
	}
	for (i32 annotation_index = 0; annotation_index < annotation_set->active_annotation_count; ++annotation_index) {
		annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);

		// stub
	}
}

void save_annotations(app_state_t* app_state, annotation_set_t* annotation_set, bool force_ignore_delay) {
	if (!annotation_set->modified) return; // no changes, nothing to do

	bool proceed = force_ignore_delay;
	if (!force_ignore_delay) {
		float seconds_since_last_modified = get_seconds_elapsed(annotation_set->last_modification_time, get_clock());
		// only autosave if there haven't been any additional changes for some time (don't do it too often)
		if (seconds_since_last_modified > 2.0f) {
			proceed = true;
		}
	}
	if (proceed) {
		if (annotation_set->export_as_asap_xml) {
			// Backup original XML file to .orig
			if (annotation_set->asap_xml_filename[0] != '\0' && annotation_set->annotations_were_loaded_from_file) {
				char backup_filename[4096];
				snprintf(backup_filename, sizeof(backup_filename), "%s.orig", annotation_set->asap_xml_filename);
				if (!file_exists(backup_filename)) {
					rename(annotation_set->asap_xml_filename, backup_filename);
				}
			}

			// Construct a sensible filename
			if (annotation_set->asap_xml_filename[0] == '\0') {
				char image_name_buf[512];
				// TODO: which image to associate the annotations with?
				if (arrlen(app_state->loaded_images) > 0) {
					image_t* image = app_state->loaded_images[0];
					strncpy(image_name_buf, image->name, MIN(sizeof(image_name_buf), sizeof(image->name)));
				} else {
					strncpy(image_name_buf, "unknown_image", sizeof(image_name_buf));
				}
				replace_file_extension(image_name_buf, sizeof(image_name_buf), "xml");
				snprintf(annotation_set->asap_xml_filename, sizeof(annotation_set->asap_xml_filename), "%s%s", get_annotation_directory(app_state), image_name_buf);
				annotation_set->asap_xml_filename[sizeof(annotation_set->asap_xml_filename)-1] = '\0';

			}

			save_asap_xml_annotations(annotation_set, annotation_set->asap_xml_filename);
		}
		if (app_state->export_as_coco && annotation_set->coco_filename) {
			char backup_filename[4096];
			snprintf(backup_filename, sizeof(backup_filename), "%s.orig", annotation_set->coco_filename);
			if (!file_exists(backup_filename)) {
				rename(annotation_set->coco_filename, backup_filename);
			}
			coco_transfer_annotations_from_annotation_set(&annotation_set->coco, annotation_set);
			memrw_t out = save_coco(&annotation_set->coco);
			// TODO: save in the background
			FILE* fp = fopen(annotation_set->coco_filename, "wb");
			if (fp) {
				fwrite(out.data, out.used_size, 1, fp);
				fclose(fp);
			}
			memrw_destroy(&out);
		}
		annotation_set->modified = false;


	}
}

void recount_selected_annotations(app_state_t* app_state, annotation_set_t* annotation_set) {
	i32 selection_count = 0;
	annotation_set->selected_annotations = (annotation_t**) arena_current_pos(&app_state->temp_arena);
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_t* annotation = get_active_annotation(annotation_set, i);
		if (annotation->selected) {
			arena_push_array(&app_state->temp_arena, 1, annotation_t*); // reserve
			annotation_set->selected_annotations[selection_count] = annotation;
			++selection_count;
		}
	}
	annotation_set->selection_count = selection_count;
}


annotation_set_t create_offsetted_annotation_set_for_area(annotation_set_t* annotation_set, bounds2f area, bool push_coordinates_inward) {
	// Create a duplicate annotation set
	annotation_set_t result_set = {};
	result_set.mpp = annotation_set->mpp;

	// Copy groups
	arrsetlen(result_set.stored_groups, annotation_set->stored_group_count);
	memcpy(result_set.stored_groups, annotation_set->stored_groups, annotation_set->stored_group_count * sizeof(annotation_group_t));
	result_set.stored_group_count = annotation_set->stored_group_count;
	arrsetlen(result_set.active_group_indices, annotation_set->active_group_count);
	memcpy(result_set.active_group_indices, annotation_set->active_group_indices, annotation_set->active_group_count * sizeof(i32));
	result_set.active_group_count = annotation_set->active_group_count;

	// Copy features
	arrsetlen(result_set.stored_features, annotation_set->stored_feature_count);
	memcpy(result_set.stored_features, annotation_set->stored_features, annotation_set->stored_feature_count * sizeof(annotation_feature_t));
	result_set.stored_feature_count = annotation_set->stored_feature_count;
	arrsetlen(result_set.active_feature_indices, annotation_set->active_feature_count);
	memcpy(result_set.active_feature_indices, annotation_set->active_feature_indices, annotation_set->active_feature_count * sizeof(i32));
	result_set.active_feature_count = annotation_set->active_feature_count;

	float area_width = area.max.x - area.min.x;
	float area_height = area.max.y - area.min.y;

	// Copy annotations
	for (i32 annotation_index = 0; annotation_index < annotation_set->active_annotation_count; ++annotation_index) {
		annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);

		// check bounds
		annotation_recalculate_bounds_if_necessary(annotation);
		if (are_bounds2f_overlapping(annotation->bounds, area)) {

			annotation_t offsetted = duplicate_annotation(annotation);
			for (i32 i = 0; i < offsetted.coordinate_count; ++i) {
				v2f* coordinate = offsetted.coordinates + i;
				coordinate->x -= area.min.x;
				coordinate->y -= area.min.y;

				if (push_coordinates_inward) {
					// 'Push' coordinates within the cropped area
					if (coordinate->x < 0.0f) coordinate->x = 0.0f;
					else if (coordinate->x > area_width) coordinate->x = area_width;
					if (coordinate->y < 0.0f) coordinate->y = 0.0f;
					else if (coordinate->y > area_height) coordinate->y = area_height;
				}
			}

			i32 new_stored_annotation_index = result_set.stored_annotation_count;
			arrput(result_set.stored_annotations, offsetted);
			result_set.stored_annotation_count++;
			arrput(result_set.active_annotation_indices, new_stored_annotation_index);
			result_set.active_annotation_count++;

		}
	}

	return result_set;
}


void annotation_set_template_destroy(annotation_set_template_t* template_) {
    arrfree(template_->groups);
    arrfree(template_->features);
}

annotation_set_template_t create_annotation_set_template(annotation_set_t* annotation_set) {
    annotation_set_template_t result = {};
    arrsetlen(result.groups, annotation_set->active_group_count);
    arrsetlen(result.features, annotation_set->active_feature_count);
    for (i32 i = 0; i < annotation_set->active_group_count; ++i) {
        result.groups[i] = *get_active_annotation_group(annotation_set, i);
    }
    for (i32 i = 0; i < annotation_set->active_feature_count; ++i) {
        result.features[i] = *get_active_annotation_feature(annotation_set, i);
    }
    result.is_valid = true;
    return result;
}

void annotation_set_init_from_template(annotation_set_t* annotation_set, annotation_set_template_t* template_) {
    arrfree(annotation_set->stored_groups);
    arrfree(annotation_set->active_group_indices);
    arrfree(annotation_set->stored_features);
    arrfree(annotation_set->active_feature_indices);
    annotation_set->stored_group_count = 0;
    annotation_set->active_group_count = 0;
    annotation_set->stored_feature_count = 0;
    annotation_set->stored_feature_count = 0;
    for (i32 i = 0; i < arrlen(template_->groups); ++i) {
        annotation_group_t* new_group = get_active_annotation_group(annotation_set, add_annotation_group(annotation_set, ""));
        *new_group = *(template_->groups + i);
    }
    for (i32 i = 0; i < arrlen(template_->features); ++i) {
        annotation_feature_t* new_feature = get_active_annotation_feature(annotation_set, add_annotation_feature(annotation_set, ""));
        *new_feature = *(template_->features + i);
    }
    if (annotation_set->active_group_count == 0) {
        add_annotation_group(annotation_set, "None");
    }
}


