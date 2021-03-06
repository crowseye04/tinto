/**************************************************************************
*
* Tint2 : taskbar
*
* Copyright (C) 2008 thierry lorthiois (lorthiois@bbsoft.fr) from Omega distribution
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License version 2
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**************************************************************************/

#include "conf.h"

#include <stdbool.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <Imlib2.h>

#include "misc.h"
#include "task.h"
#include "taskbar.h"
#include "server.h"
#include "window.h"
#include "panel.h"
#include "strnatcmp.h"


/* win_to_task_table holds for every Window an array of tasks. Usually the array contains only one
   element. However for omnipresent windows (windows which are visible in every taskbar) the array
   contains to every Task* on each panel a pointer (i.e. GPtrArray.len == server.nb_desktop)
*/
GHashTable* win_to_task_table;

Task *task_active;
Task *task_drag;
int taskbar_enabled;
int taskbar_distribute_size;
int hide_inactive_tasks;
int hide_task_diff_monitor;
int taskbar_sort_method;

guint win_hash(gconstpointer key) { return (guint)*((Window*)key); }
gboolean win_compare(gconstpointer a, gconstpointer b) { return (*((Window*)a) == *((Window*)b)); }
void free_ptr_array(gpointer data) { g_ptr_array_free(data, 1); }

static bool contained_within (Task *a, Task *b);

void default_taskbar()
{
	win_to_task_table = NULL;
	urgent_timeout = NULL;
	urgent_list = NULL;
	taskbar_enabled = 0;
	taskbar_distribute_size = 0;
	hide_inactive_tasks = 0;
	hide_task_diff_monitor = 0;
	taskbar_sort_method = TASKBAR_NOSORT;
	default_taskbarname();
}

void cleanup_taskbar()
{
	Panel *panel;
	Taskbar *tskbar;
	int i, j, k;

	cleanup_taskbarname();
	if (win_to_task_table) {
		while (g_hash_table_size(win_to_task_table)) {
			GHashTableIter iter;
			gpointer key, value;

			g_hash_table_iter_init(&iter, win_to_task_table);
			if (g_hash_table_iter_next(&iter, &key, &value)) {
				taskbar_remove_task(key, 0, 0);
			}
		}
		g_hash_table_destroy(win_to_task_table);
		win_to_task_table = NULL;
	}
	for (i = 0 ; i < nb_panel; i++) {
		panel = &panel1[i];
		for (j = 0; j < panel->nb_desktop; j++) {
			tskbar = &panel->taskbar[j];
			for (k = 0; k < TASKBAR_STATE_COUNT; ++k) {
				if (tskbar->state_pix[k])
					XFreePixmap(server.dsp, tskbar->state_pix[k]);
				tskbar->state_pix[k] = 0;
			}
			area_destroy (&tskbar->area);
			// remove taskbar from the panel
			panel->area.children = g_slist_remove(panel->area.children, tskbar);
		}
		if (panel->taskbar) {
			free(panel->taskbar);
			panel->taskbar = NULL;
		}
	}

	g_slist_free(urgent_list);
	urgent_list = NULL;

	stop_timeout(urgent_timeout);
}


void init_taskbar()
{
	if (win_to_task_table == 0)
		win_to_task_table = g_hash_table_new_full(win_hash, win_compare, free, free_ptr_array);

	task_active = 0;
	task_drag = 0;
}


void init_taskbar_panel(void *p)
{
	Panel *panel =(Panel*)p;
	int j;

	if (panel->g_taskbar.background[TASKBAR_NORMAL] == 0) {
		panel->g_taskbar.background[TASKBAR_NORMAL] = &g_array_index(backgrounds, background_t, 0);
		panel->g_taskbar.background[TASKBAR_ACTIVE] = &g_array_index(backgrounds, background_t, 0);
	}
	if (panel->g_taskbar.background_name[TASKBAR_NORMAL] == 0) {
		panel->g_taskbar.background_name[TASKBAR_NORMAL] = &g_array_index(backgrounds, background_t, 0);
		panel->g_taskbar.background_name[TASKBAR_ACTIVE] = &g_array_index(backgrounds, background_t, 0);
	}
	if (!panel->g_task.font_desc)
		panel->g_task.font_desc = pango_font_description_from_string(DEFAULT_FONT);
	if (panel->g_task.area.background == 0)
		panel->g_task.area.background = &g_array_index(backgrounds, background_t, 0);

	// taskbar name
	panel->g_taskbar.area_name.panel = panel;
	panel->g_taskbar.area_name.size_mode = SIZE_BY_CONTENT;
	panel->g_taskbar.area_name._resize = resize_taskbarname;
	panel->g_taskbar.area_name.area_draw_foreground = draw_taskbarname;
	panel->g_taskbar.area_name._on_change_layout = 0;
	panel->g_taskbar.area_name.resize = 1;
	panel->g_taskbar.area_name.visible = 1;

	// taskbar
	panel->g_taskbar.area.parent = panel;
	panel->g_taskbar.area.panel = panel;
	panel->g_taskbar.area.size_mode = SIZE_BY_LAYOUT;
	panel->g_taskbar.area._resize = resize_taskbar;
	panel->g_taskbar.area.area_draw_foreground = draw_taskbar;
	panel->g_taskbar.area._on_change_layout = on_change_taskbar;
	panel->g_taskbar.area.resize = 1;
	panel->g_taskbar.area.visible = 1;
	if (panel_horizontal) {
		panel->g_taskbar.area.bounds.y = panel->area.background->border.width + panel->area.paddingy;
		panel->g_taskbar.area.bounds.height = panel->area.bounds.height - (2 * panel->g_taskbar.area.bounds.y);
		panel->g_taskbar.area_name.bounds.y = panel->g_taskbar.area.bounds.y;
		panel->g_taskbar.area_name.bounds.height = panel->g_taskbar.area.bounds.height;
	}
	else {
		panel->g_taskbar.area.bounds.x = panel->area.background->border.width + panel->area.paddingy;
		panel->g_taskbar.area.bounds.width = panel->area.bounds.width - (2 * panel->g_taskbar.area.bounds.x);
		panel->g_taskbar.area_name.bounds.x = panel->g_taskbar.area.bounds.x;
		panel->g_taskbar.area_name.bounds.width = panel->g_taskbar.area.bounds.width;
	}

	// task
	panel->g_task.area.panel = panel;
	panel->g_task.area.size_mode = SIZE_BY_LAYOUT;
	panel->g_task.area.area_draw_foreground = draw_task;
	panel->g_task.area._on_change_layout = on_change_task;
	panel->g_task.area.resize = 1;
	panel->g_task.area.visible = 1;
	if ((panel->g_task.config_asb_mask & (1<<TASK_NORMAL)) == 0) {
		panel->g_task.alpha[TASK_NORMAL] = 100;
		panel->g_task.saturation[TASK_NORMAL] = 0;
		panel->g_task.brightness[TASK_NORMAL] = 0;
	}
	if ((panel->g_task.config_asb_mask & (1<<TASK_ACTIVE)) == 0) {
		panel->g_task.alpha[TASK_ACTIVE] = panel->g_task.alpha[TASK_NORMAL];
		panel->g_task.saturation[TASK_ACTIVE] = panel->g_task.saturation[TASK_NORMAL];
		panel->g_task.brightness[TASK_ACTIVE] = panel->g_task.brightness[TASK_NORMAL];
	}
	if ((panel->g_task.config_asb_mask & (1<<TASK_ICONIFIED)) == 0) {
		panel->g_task.alpha[TASK_ICONIFIED] = panel->g_task.alpha[TASK_NORMAL];
		panel->g_task.saturation[TASK_ICONIFIED] = panel->g_task.saturation[TASK_NORMAL];
		panel->g_task.brightness[TASK_ICONIFIED] = panel->g_task.brightness[TASK_NORMAL];
	}
	if ((panel->g_task.config_asb_mask & (1<<TASK_URGENT)) == 0) {
		panel->g_task.alpha[TASK_URGENT] = panel->g_task.alpha[TASK_ACTIVE];
		panel->g_task.saturation[TASK_URGENT] = panel->g_task.saturation[TASK_ACTIVE];
		panel->g_task.brightness[TASK_URGENT] = panel->g_task.brightness[TASK_ACTIVE];
	}
	if ((panel->g_task.config_font_mask & (1<<TASK_NORMAL)) == 0) panel->g_task.font_colors[TASK_NORMAL] = (color_rgba_t){0, 0, 0, UINT8_MAX};
	if ((panel->g_task.config_font_mask & (1<<TASK_ACTIVE)) == 0) panel->g_task.font_colors[TASK_ACTIVE] = panel->g_task.font_colors[TASK_NORMAL];
	if ((panel->g_task.config_font_mask & (1<<TASK_ICONIFIED)) == 0) panel->g_task.font_colors[TASK_ICONIFIED] = panel->g_task.font_colors[TASK_NORMAL];
	if ((panel->g_task.config_font_mask & (1<<TASK_URGENT)) == 0) panel->g_task.font_colors[TASK_URGENT] = panel->g_task.font_colors[TASK_ACTIVE];
	if ((panel->g_task.config_background_mask & (1<<TASK_NORMAL)) == 0) panel->g_task.background[TASK_NORMAL] = &g_array_index(backgrounds, background_t, 0);
	if ((panel->g_task.config_background_mask & (1<<TASK_ACTIVE)) == 0) panel->g_task.background[TASK_ACTIVE] = panel->g_task.background[TASK_NORMAL];
	if ((panel->g_task.config_background_mask & (1<<TASK_ICONIFIED)) == 0) panel->g_task.background[TASK_ICONIFIED] = panel->g_task.background[TASK_NORMAL];
	if ((panel->g_task.config_background_mask & (1<<TASK_URGENT)) == 0) panel->g_task.background[TASK_URGENT] = panel->g_task.background[TASK_ACTIVE];

	if (panel_horizontal) {
		panel->g_task.area.bounds.y = panel->g_taskbar.area.bounds.y + panel->g_taskbar.background[TASKBAR_NORMAL]->border.width + panel->g_taskbar.area.paddingy;
		panel->g_task.area.bounds.height = panel->area.bounds.height - (2 * panel->g_task.area.bounds.y);
	}
	else {
		panel->g_task.area.bounds.x = panel->g_taskbar.area.bounds.x + panel->g_taskbar.background[TASKBAR_NORMAL]->border.width + panel->g_taskbar.area.paddingy;
		panel->g_task.area.bounds.width = panel->area.bounds.width - (2 * panel->g_task.area.bounds.x);
		panel->g_task.area.bounds.height = panel->g_task.maximum_height;
	}

	for (j=0; j<TASK_STATE_COUNT; ++j) {
		if (panel->g_task.background[j] == 0)
			panel->g_task.background[j] = &g_array_index(backgrounds, background_t, 0);
    if (panel->g_task.background[j]->border.radius >
	panel->g_task.area.bounds.height / 2) {
			g_array_append_val(backgrounds, *panel->g_task.background[j]);
			panel->g_task.background[j] = &g_array_index(backgrounds, background_t, backgrounds->len-1);
      panel->g_task.background[j]->border.radius =
	panel->g_task.area.bounds.height / 2;
    }
	}

	// compute vertical position : text and icon
	int height_ink, height;
	get_text_size(panel->g_task.font_desc, &height_ink, &height, panel->area.bounds.height, "TAjpg", 5);

	if (!panel->g_task.maximum_width && panel_horizontal)
		panel->g_task.maximum_width = server.monitor[panel->monitor].width;

	panel->g_task.text_posx = panel->g_task.background[0]->border.width + panel->g_task.area.paddingxlr;
	panel->g_task.text_height = panel->g_task.area.bounds.height - (2 * panel->g_task.area.paddingy);
	if (panel->g_task.icon) {
		panel->g_task.icon_size1 = panel->g_task.area.bounds.height - (2 * panel->g_task.area.paddingy);
		panel->g_task.text_posx += panel->g_task.icon_size1;
		panel->g_task.icon_posy = (panel->g_task.area.bounds.height - panel->g_task.icon_size1) / 2;
	}
	//printf("monitor %d, task_maximum_width %d\n", panel->monitor, panel->g_task.maximum_width);

	Taskbar *tskbar;
	panel->nb_desktop = server.nb_desktop;
	panel->taskbar = calloc(server.nb_desktop, sizeof(Taskbar));
	for (j=0 ; j < panel->nb_desktop ; j++) {
		tskbar = &panel->taskbar[j];
		memcpy(&tskbar->area, &panel->g_taskbar.area, sizeof(Area));
		tskbar->desktop = j;
		if (j == server.desktop)
			tskbar->area.background = panel->g_taskbar.background[TASKBAR_ACTIVE];
		else
			tskbar->area.background = panel->g_taskbar.background[TASKBAR_NORMAL];
	}
	init_taskbarname_panel(panel);
}


void taskbar_remove_task(gpointer key, gpointer value, gpointer user_data) {
  UNUSED (value, user_data);

	remove_task(task_get_task(*(Window*)key));
}


Task *task_get_task (Window win)
{
	GPtrArray* task_group = task_get_tasks(win);
	if (task_group)
		return g_ptr_array_index(task_group, 0);
	else
		return 0;
}


GPtrArray* task_get_tasks(Window win)
{
	if (win_to_task_table && taskbar_enabled)
		return g_hash_table_lookup(win_to_task_table, &win);
	else
		return 0;
}


void task_refresh_tasklist ()
{
	Window *win;
	int num_results, i;

	if (!taskbar_enabled) return;
	win = server_get_property (server.root_win, server.atom._NET_CLIENT_LIST, XA_WINDOW, &num_results);
	if (!win) return;

	GList* win_list = g_hash_table_get_keys(win_to_task_table);
	GList* it;
	for (it=win_list; it; it=it->next) {
		for (i = 0; i < num_results; i++)
			if (*((Window*)it->data) == win[i])
				break;
		if (i == num_results)
			taskbar_remove_task(it->data, 0, 0);
	}
	g_list_free(win_list);

	// Add any new
	for (i = 0; i < num_results; i++)
		if (!task_get_task (win[i]))
			add_task (win[i]);

	XFree (win);
}


void draw_taskbar (void *obj, cairo_t *c) {
  UNUSED (c);
	Taskbar *taskbar = obj;
	int state = (taskbar->desktop == server.desktop) ? TASKBAR_ACTIVE : TASKBAR_NORMAL;

	taskbar->state_pix[state] = taskbar->area.pixmap;
}


int resize_taskbar(void *obj)
{
	Taskbar *taskbar = (Taskbar*)obj;
	Panel *panel = (Panel*)taskbar->area.panel;
	int text_width;

	//printf("resize_taskbar %d %d\n", taskbar->area.posx, taskbar->area.posy);
	if (panel_horizontal) {
		resize_by_layout(obj, panel->g_task.maximum_width);

		text_width = panel->g_task.maximum_width;
		GSList *l = taskbar->area.children;
		if (taskbarname_enabled) l = l->next;
		for (; l != NULL; l = l->next) {
			if (((Task *)l->data)->area.visible) {
				text_width = ((Task *)l->data)->area.bounds.width;
				break;
			}
		}
		taskbar->text_width = text_width - panel->g_task.text_posx - panel->g_task.area.background->border.width - panel->g_task.area.paddingx;
	}
	else {
		resize_by_layout(obj, panel->g_task.maximum_height);

		taskbar->text_width = taskbar->area.bounds.width - (2 * panel->g_taskbar.area.paddingy) - panel->g_task.text_posx -	panel->g_task.area.background->border.width - panel->g_task.area.paddingx;
	}
	return 0;
}


void on_change_taskbar (void *obj)
{
	Taskbar *tskbar = obj;
	int k;

	// reset Pixmap when position/size changed
	for (k=0; k<TASKBAR_STATE_COUNT; ++k) {
		if (tskbar->state_pix[k]) XFreePixmap(server.dsp, tskbar->state_pix[k]);
		tskbar->state_pix[k] = 0;
	}
	tskbar->area.pixmap = 0;
	tskbar->area.redraw = 1;
}


void set_taskbar_state(Taskbar *tskbar, int state)
{
	tskbar->area.background = panel1[0].g_taskbar.background[state];
	tskbar->area.pixmap = tskbar->state_pix[state];
	if (taskbarname_enabled) {
		tskbar->bar_name.area.background = panel1[0].g_taskbar.background_name[state];
		tskbar->bar_name.area.pixmap = tskbar->bar_name.state_pix[state];
	}
	if (panel_mode != MULTI_DESKTOP) {
		if (state == TASKBAR_NORMAL)
			tskbar->area.visible = 0;
		else
			tskbar->area.visible = 1;
	}
	if (tskbar->area.visible == 1) {
		if (tskbar->state_pix[state] == 0)
			tskbar->area.redraw = 1;
		if (taskbarname_enabled && tskbar->bar_name.state_pix[state] == 0)
			tskbar->bar_name.area.redraw = 1;
		if (panel_mode == MULTI_DESKTOP && panel1[0].g_taskbar.background[TASKBAR_NORMAL] != panel1[0].g_taskbar.background[TASKBAR_ACTIVE]) {
			GSList *l = tskbar->area.children;
			if (taskbarname_enabled) l = l->next;
			for ( ; l ; l = l->next)
				set_task_redraw(l->data);
		}
	}
	panel_refresh = 1;
}


void visible_taskbar(void *p)
{
	Panel *panel =(Panel*)p;
	int j;

	Taskbar *taskbar;
	for (j=0 ; j < panel->nb_desktop ; j++) {
		taskbar = &panel->taskbar[j];
		if (panel_mode != MULTI_DESKTOP && taskbar->desktop != server.desktop) {
			// SINGLE_DESKTOP and not current desktop
			taskbar->area.visible = 0;
		}
		else {
			taskbar->area.visible = 1;
		}
	}
	panel_refresh = 1;
}

#define NONTRIVIAL 2
gint compare_tasks_trivial(Task *a, Task *b, Taskbar *taskbar)
{
	if (a == b)
		return 0;
	if (taskbarname_enabled) {
		if (a == taskbar->area.children->data)
			return -1;
		if (b == taskbar->area.children->data)
			return 1;
	}
	return NONTRIVIAL;
}

static bool contained_within (Task *a, Task *b) {
  if ((a->geometry.x <= b->geometry.x)
      && (a->geometry.y <= b->geometry.y)
      && (a->geometry.x + a->geometry.width >= b->geometry.x + b->geometry.width)
      && (a->geometry.y + a->geometry.height >= b->geometry.y + b->geometry.height)) {
    return true;
  }

  return false;
}

gint compare_task_centers(Task *a, Task *b, Taskbar *taskbar)
{
	int trivial = compare_tasks_trivial(a, b, taskbar);
	if (trivial != NONTRIVIAL)
		return trivial;

  // If a window has the same coordinates and size as the other,
  // they are considered to be equal in the comparison.
  if (rect_equals (&a->geometry, &b->geometry)) return 0;

  // If a window is completely contained in another,
  // then it is considered to come after (to the right/bottom) of the other.
  if (contained_within (a, b)) return -1;
  else if (contained_within (b, a)) return 1;

  // Compare centers
  const int a_horiz_c = a->geometry.x + a->geometry.width / 2;
  const int b_horiz_c = b->geometry.x + b->geometry.width / 2;
  const int a_vert_c = a->geometry.y + a->geometry.height / 2;
  const int b_vert_c = b->geometry.y + b->geometry.height / 2;
	if (panel_horizontal) {
		if (a_horiz_c != b_horiz_c) {
			return a_horiz_c - b_horiz_c;
		}
		return a_vert_c - b_vert_c;
	} else {
		if (a_vert_c != b_vert_c) {
			return a_vert_c - b_vert_c;
		}
		return a_horiz_c - b_horiz_c;
	}
}

gint compare_task_titles(Task *a, Task *b, Taskbar *taskbar)
{
	int trivial = compare_tasks_trivial(a, b, taskbar);
	if (trivial != NONTRIVIAL)
		return trivial;
	return strnatcasecmp(a->title ? a->title : "", b->title ? b->title : "");
}

gint compare_tasks(Task *a, Task *b, Taskbar *taskbar)
{
	int trivial = compare_tasks_trivial(a, b, taskbar);
	if (trivial != NONTRIVIAL)
		return trivial;
	if (taskbar_sort_method == TASKBAR_NOSORT) {
		return 0;
	} else if (taskbar_sort_method == TASKBAR_SORT_CENTER) {
		return compare_task_centers(a, b, taskbar);
	} else if (taskbar_sort_method == TASKBAR_SORT_TITLE) {
		return compare_task_titles(a, b, taskbar);
	}
	return 0;
}

int taskbar_needs_sort(Taskbar *taskbar)
{
	if (taskbar_sort_method == TASKBAR_NOSORT)
		return 0;

	GSList *i, *j;
	for (i = taskbar->area.children, j = i ? i->next : NULL; i && j; i = i->next, j = j->next) {
		if (compare_tasks(i->data, j->data, taskbar) > 0) {
			return 1;
		}
	}

	return 0;
}

void sort_tasks(Taskbar *taskbar)
{
	if (!taskbar)
		return;
	if (!taskbar_needs_sort(taskbar)) {
		return;
	}
	taskbar->area.children = g_slist_sort_with_data(taskbar->area.children, (GCompareDataFunc)compare_tasks, taskbar);
	taskbar->area.resize = 1;
	panel_refresh = 1;
	((Panel*)taskbar->area.panel)->area.resize = 1;
}


void sort_taskbar_for_win(Window win)
{
	if (taskbar_sort_method == TASKBAR_NOSORT)
		return;

	GPtrArray* task_group = task_get_tasks(win);
	if (task_group) {
		Task* tsk0 = g_ptr_array_index(task_group, 0);

    if (tsk0) window_get_coordinates (win, &tsk0->geometry);
    Task* tsk = NULL;
    for (size_t i = 0; i < task_group->len; ++i) {
      tsk = g_ptr_array_index(task_group, i);
      tsk->geometry = tsk0->geometry;
      sort_tasks(tsk->area.parent);
    }
  }
}
