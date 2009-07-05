#include "settings.h"
#include "builtin-widgets.h"

static int create_widget_private(struct widget *w, struct config_format_entry *e, 
		struct config_format_tree *tree);
static void destroy_widget_private(struct widget *w);
static void draw(struct widget *w);
static void button_click(struct widget *w, XButtonEvent *e);
static void prop_change(struct widget *w, XPropertyEvent *e);
static void client_msg(struct widget *w, XClientMessageEvent *e);

static void dnd_start(struct widget *w, struct drag_info *di);
static void dnd_drag(struct widget *w, struct drag_info *di);
static void dnd_drop(struct widget *w, struct drag_info *di);

struct widget_interface taskbar_interface = {
	.theme_name 		= "taskbar",
	.size_type 		= WIDGET_SIZE_FILL,
	.create_widget_private 	= create_widget_private,
	.destroy_widget_private = destroy_widget_private,
	.draw 			= draw,
	.button_click 		= button_click,
	.prop_change 		= prop_change,
	.dnd_start 		= dnd_start,
	.dnd_drag 		= dnd_drag,
	.dnd_drop 		= dnd_drop,
	.client_msg		= client_msg
};

/**************************************************************************
  Taskbar theme
**************************************************************************/

static int parse_taskbar_state(struct taskbar_state *ts, const char *name,
		struct config_format_entry *e, struct config_format_tree *tree)
{
	struct config_format_entry *ee = find_config_format_entry(e, name);
	if (!ee) {
		required_entry_not_found(e, name);
		return -1;
	}

	if (parse_triple_image(&ts->background, ee, tree, 1))
		goto parse_taskbar_state_error_background;

	if (parse_text_info_named(&ts->font, "font", ee, 1))
		goto parse_taskbar_state_error_font;

	return 0;

parse_taskbar_state_error_font:
	free_triple_image(&ts->background);
parse_taskbar_state_error_background:
	return -1;
}

static void free_taskbar_state(struct taskbar_state *ts)
{
	free_triple_image(&ts->background);
	free_text_info(&ts->font);
}

static int parse_taskbar_theme(struct taskbar_theme *tt, 
		struct config_format_entry *e, struct config_format_tree *tree)
{
	if (parse_taskbar_state(&tt->idle, "idle", e, tree))
		goto parse_taskbar_button_theme_error_idle;

	if (parse_taskbar_state(&tt->pressed, "pressed", e, tree))
		goto parse_taskbar_button_theme_error_pressed;

	struct config_format_entry *ee = find_config_format_entry(e, "default_icon");
	if (ee) {
		tt->default_icon = parse_image_part(ee, tree, 0);
		tt->icon_offset_x = tt->icon_offset_y = 0;
		parse_2ints(&tt->icon_offset_x, &tt->icon_offset_y, "offset", ee);
		tt->icon_align = parse_align("align", ee);
		if ( tt->icon_align == ALIGN_CENTER ) {
			/* Not a correct value, fallback to default */
			tt->icon_align = ALIGN_LEFT;
		}
	}

	tt->separator = parse_image_part_named("separator", e, tree, 0);
	tt->task_max_width = parse_int("task_max_width", e, 0);

	return 0;

parse_taskbar_button_theme_error_pressed:
	free_taskbar_state(&tt->idle);
parse_taskbar_button_theme_error_idle:
	return -1;
}

static void free_taskbar_theme(struct taskbar_theme *tt)
{
	free_taskbar_state(&tt->idle);
	free_taskbar_state(&tt->pressed);
	if (tt->default_icon)
		cairo_surface_destroy(tt->default_icon);
	if (tt->separator)
		cairo_surface_destroy(tt->separator);
}

/**************************************************************************
  Taskbar task management
**************************************************************************/

static int find_task_by_window(struct taskbar_widget *tw, Window win)
{
	size_t i;
	for (i = 0; i < tw->tasks_n; ++i) {
		if (tw->tasks[i].win == win)
			return (int)i;
	}
	return -1;
}

static int find_last_task_by_desktop(struct taskbar_widget *tw, int desktop)
{
	int t = -1;
	size_t i;
	for (i = 0; i < tw->tasks_n; ++i) {
		if (tw->tasks[i].desktop <= desktop)
			t = (int)i;
	}
	return t;
}

static void add_task(struct widget *w, struct x_connection *c, Window win)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct taskbar_task t;

	x_set_error_trap();
	if (x_is_window_hidden(c, win)) {
		if (x_done_error_trap())
			return;
		// we need this if window will apear later
		if (w->panel->win != win)
			XSelectInput(c->dpy, win, PropertyChangeMask);
		return;
	}
	
	XSelectInput(c->dpy, win, PropertyChangeMask);

	CLEAR_STRUCT(&t);
	t.win = win;

	x_realloc_window_name(&t.name, c, win, &t.name_atom, &t.name_type_atom); 
	if (tw->theme.default_icon)
		t.icon = get_window_icon(c, win, tw->theme.default_icon);
	else
		t.icon = 0;
	t.desktop = x_get_window_desktop(c, win);

	int i = find_last_task_by_desktop(tw, t.desktop);
	if (i == -1)
		ARRAY_APPEND(tw->tasks, t);
	else
		ARRAY_INSERT_AFTER(tw->tasks, (size_t)i, t);
}

static void free_task(struct taskbar_task *t)
{
	strbuf_free(&t->name);
	if (t->icon)
		cairo_surface_destroy(t->icon);
}

static void remove_task(struct taskbar_widget *tw, size_t i)
{
	free_task(&tw->tasks[i]);
	ARRAY_REMOVE(tw->tasks, i);
}

static void free_tasks(struct taskbar_widget *tw)
{
	size_t i;
	for (i = 0; i < tw->tasks_n; ++i)
		free_task(&tw->tasks[i]);
	FREE_ARRAY(tw->tasks);
}

static int count_tasks_on_desktop(struct taskbar_widget *tw, int desktop)
{
	int count = 0;
	size_t i;
	for (i = 0; i < tw->tasks_n; ++i) {
		if (tw->tasks[i].desktop == desktop ||
		    tw->tasks[i].desktop == -1) /* count "all desktops" too */
		{
			count++;
		}
	}
	return count;
}

static void draw_task(struct taskbar_task *task, struct taskbar_theme *theme,
		cairo_t *cr, PangoLayout *layout, short active)
{
	/* calculations */
	struct triple_image *tbt = (active) ? 
			&theme->pressed.background :
			&theme->idle.background;
	struct text_info *font = (active) ?
			&theme->pressed.font :
			&theme->idle.font;
	
	int leftw = image_width(tbt->left);
	int rightw = image_width(tbt->right);
	int centerw = task->w - leftw - rightw;
	
	/* background */
	if (leftw)
		blit_image(tbt->left, cr, task->x, task->y);
	pattern_image(tbt->center, cr, task->x + leftw, task->y, centerw, task->h);
	if (rightw)
		blit_image(tbt->right, cr, task->x + leftw + centerw, task->y);

	int textx = task->x + leftw;
	int texty = task->y;
	int textw = centerw;
	int texth = task->h;

	/* icon */
	int iconw = image_width(theme->default_icon) + theme->icon_offset_x;
	int iconh = image_height(theme->default_icon) + theme->icon_offset_y;
	if (iconw && iconh) {
		int iconx = task->x + leftw + theme->icon_offset_x;
		int icony = task->y + theme->icon_offset_y;

		if ( theme->icon_align == ALIGN_LEFT || theme->icon_align == ALIGN_RIGHT ) {
			icony += (task->h - iconh) / 2;
			if ( theme->icon_align == ALIGN_RIGHT )
				iconx = (task->x + leftw + centerw) - iconw;
			textw -= iconw;
			if ( theme->icon_align == ALIGN_LEFT )
				textx += iconw;
		}
		if ( theme->icon_align == ALIGN_BOTTOM || theme->icon_align == ALIGN_TOP ) {
			if ( theme->icon_align == ALIGN_BOTTOM )
				icony = (task->y + task->h) - iconh;
			if ( theme->icon_align == ALIGN_TOP )
				texty += iconh;
			texth -= iconh;
		}

		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		blit_image(task->icon, cr, iconx, icony);
		cairo_restore(cr);
	}
	
	/* text */
	draw_text(cr, layout, font, task->name.buf, textx, texty, textw, texth);
}

static inline void activate_task(struct x_connection *c, struct taskbar_task *t)
{
	x_send_netwm_message(c, t->win, c->atoms[XATOM_NET_ACTIVE_WINDOW], 
			2, CurrentTime, 0, 0, 0);

	XWindowChanges wc;
	wc.stack_mode = Above;
	XConfigureWindow(c->dpy, t->win, CWStackMode, &wc);
}

static inline void close_task(struct x_connection *c, struct taskbar_task *t)
{
	x_send_netwm_message(c, t->win, c->atoms[XATOM_NET_CLOSE_WINDOW],
			CurrentTime, 2, 0, 0, 0);
}

static void move_task(struct taskbar_widget *tw, int what, int where)
{
	struct taskbar_task t = tw->tasks[what];
	if (what == where)
		return;
	ARRAY_REMOVE(tw->tasks, (size_t)what);
	if (where > what) {
		where -= 1;
		ARRAY_INSERT_AFTER(tw->tasks, (size_t)where, t);
	} else {
		ARRAY_INSERT_BEFORE(tw->tasks, (size_t)where, t);
	}
}

/**************************************************************************
  Updates
**************************************************************************/

static void update_active(struct taskbar_widget *tw, struct x_connection *c)
{
	tw->active = x_get_prop_window(c, c->root, 
			c->atoms[XATOM_NET_ACTIVE_WINDOW]);
}

static void update_desktop(struct taskbar_widget *tw, struct x_connection *c)
{
	tw->desktop = x_get_prop_int(c, c->root, 
			c->atoms[XATOM_NET_CURRENT_DESKTOP]);
}

static void update_tasks(struct widget *w, struct x_connection *c)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	Window *wins;
	int num;

	wins = x_get_prop_data(c, c->root, c->atoms[XATOM_NET_CLIENT_LIST], 
			XA_WINDOW, &num);

	size_t i;
	int j;
	for (i = 0; i < tw->tasks_n; ++i) {
		struct taskbar_task *t = &tw->tasks[i];
		int delete = 1;
		for (j = 0; j < num; ++j) {
			if (wins[j] == t->win) {
				delete = 0;
				break;
			}
		}
		if (delete)
			remove_task(tw, i--);
	}

	for (j = 0; j < num; ++j) {
		if (find_task_by_window(tw, wins[j]) == -1)
			add_task(w, c, wins[j]);
	}
	
	XFree(wins);
}

/**************************************************************************
  Taskbar interface
**************************************************************************/

static int get_taskbar_task_at(struct widget *w, int x, int y)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;

	size_t i;
	for (i = 0; i < tw->tasks_n; ++i) {
		struct taskbar_task *t = &tw->tasks[i];
		if (t->desktop != tw->desktop &&
		    t->desktop != -1)
		{
			continue;
		}

		if (x < (t->x + t->w) && x >= t->x && y < (t->y + t->h) && y >= t->y)
			return (int)i;
	}
	return -1;
}

static int create_widget_private(struct widget *w, struct config_format_entry *e, 
		struct config_format_tree *tree)
{
	struct taskbar_widget *tw = xmallocz(sizeof(struct taskbar_widget));
	if (parse_taskbar_theme(&tw->theme, e, tree)) {
		xfree(tw);
		XWARNING("Failed to parse taskbar theme");
		return -1;
	}

	if (w->panel->theme.vertical) {
		w->width = w->panel->width;
	} else {
		w->height = w->panel->height;
	}

	INIT_ARRAY(tw->tasks, 50);
	w->private = tw;

	struct x_connection *c = &w->panel->connection;
	update_desktop(tw, c);
	update_active(tw, c);
	update_tasks(w, c);
	tw->dnd_win = None;
	tw->taken = None;
	tw->task_death_threshold = parse_int("task_death_threshold", 
					     &g_settings.root, 50);
	tw->dnd_cur = XCreateFontCursor(c->dpy, XC_fleur);

	return 0;
}

static void destroy_widget_private(struct widget *w)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	free_taskbar_theme(&tw->theme);
	free_tasks(tw);
	XFreeCursor(w->panel->connection.dpy, tw->dnd_cur);
	xfree(tw);
}

static void draw(struct widget *w)
{
	/* I think it's a good idea to calculate all buttons positions here, and
	 * cache these data for later use in other message handlers. User
	 * interacts with what he/she sees, right? 
	 */
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct panel *p = w->panel;
	struct x_connection *c = &p->connection;
	cairo_t *cr = p->cr;
	short vertical = p->theme.vertical;

	int count = count_tasks_on_desktop(tw, tw->desktop);
	if (!count)
		return;

	int taskw;
	if ( vertical ) {
		taskw = w->width;
	} else {
		taskw = w->width / count;
		if (tw->theme.task_max_width && taskw > tw->theme.task_max_width)
			taskw = tw->theme.task_max_width;
	}

	int x = w->x;
	int y = w->y;
	
	size_t n = 0; /* number of task visible */
	size_t i;
	for (i = 0; i < tw->tasks_n; ++i) {
		struct taskbar_task *t = &tw->tasks[i];
		short active = (t->win == tw->active);
		
		if (t->desktop != tw->desktop &&
		    t->desktop != -1)
		{
			continue;
		}
		n++; 

		/* last task width correction */
		if ( !vertical ) {
			if (n == count)
				taskw = (w->x + w->width) - x;
		}
		
		/* save position for other events */
		t->x = x;
		t->y = y;
		t->w = taskw;
		t->h = image_height (active ? tw->theme.pressed.background.center : tw->theme.idle.background.center);

		/* set icon geometry */
		if (t->geom_x != t->x || t->geom_y != t->y || t->geom_w != t->w) {
			t->geom_x = t->x;
			t->geom_y = t->y;
			t->geom_w = t->w;
			t->geom_h = t->h;

			long icon_geometry[4] = {
				w->panel->x + t->geom_x,
				w->panel->y + t->geom_y,
				t->geom_w,
				t->geom_h
			};
			x_set_prop_array(c, t->win, c->atoms[XATOM_NET_WM_ICON_GEOMETRY],
					 icon_geometry, 4);
		}

		draw_task(t, &tw->theme, cr, w->panel->layout, active);
		if ( vertical )
			y += t->h;
		else
			x += t->w;
	}
}

static void prop_change(struct widget *w, XPropertyEvent *e)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct x_connection *c = &w->panel->connection;

	/* root window props */
	if (e->window == c->root) {
		if (e->atom == c->atoms[XATOM_NET_ACTIVE_WINDOW]) {
			update_active(tw, c);
			w->needs_expose = 1;
			return;
		}
		if (e->atom == c->atoms[XATOM_NET_CURRENT_DESKTOP]) {
			update_desktop(tw, c);
			w->needs_expose = 1;
			return;
		}
		if (e->atom == c->atoms[XATOM_NET_CLIENT_LIST]) {
			update_tasks(w, c);
			w->needs_expose = 1;
			return;
		}
	}

	/* check if it's our task */
	int ti = find_task_by_window(tw, e->window);
	if (ti == -1) {
		if (e->atom == c->atoms[XATOM_NET_WM_STATE] ||
		    e->atom == c->atoms[XATOM_NET_WM_WINDOW_TYPE]) {
			add_task(w, c, e->window);
		}
		return;
	}

	/* desktop changed (task was moved to other desktop) */
	if (e->atom == c->atoms[XATOM_NET_WM_DESKTOP]) {
		struct taskbar_task t = tw->tasks[ti];
		t.desktop = x_get_window_desktop(c, t.win);

		ARRAY_REMOVE(tw->tasks, (size_t)ti);
		int insert_after = find_last_task_by_desktop(tw, t.desktop);
		if (insert_after == -1)
			ARRAY_APPEND(tw->tasks, t);
		else
			ARRAY_INSERT_AFTER(tw->tasks, (size_t)insert_after, t);
		w->needs_expose = 1;
		return;
	}

	/* task name was changed */
	if (e->atom == tw->tasks[ti].name_atom)
	{
		struct taskbar_task *t = &tw->tasks[ti];
		x_realloc_window_name(&t->name, c, t->win, 
				    &t->name_atom, &t->name_type_atom);
		w->needs_expose = 1;
		return;
	}

	/* icon was changed */
	if (tw->theme.default_icon) {
		if (e->atom == c->atoms[XATOM_NET_WM_ICON] ||
		    e->atom == XA_WM_HINTS)
		{
			struct taskbar_task *t = &tw->tasks[ti];
			cairo_surface_destroy(t->icon);
			t->icon = get_window_icon(c, t->win, tw->theme.default_icon);
			w->needs_expose = 1;
			return;
		}
	}
}

static void button_click(struct widget *w, XButtonEvent *e)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	int ti = get_taskbar_task_at(w, e->x, e->y);
	if (ti == -1)
		return;
	struct taskbar_task *t = &tw->tasks[ti];
	struct x_connection *c = &w->panel->connection;

	if (e->button == 1 && e->type == ButtonRelease) {
		if (tw->active == t->win)
			XIconifyWindow(c->dpy, t->win, c->screen);
		else
			activate_task(c, t);
	}
}

static Window create_window_for_dnd(struct x_connection *c, int x, int y,
				    cairo_surface_t *icon)
{
	int iconw = image_width(icon);
	int iconh = image_width(icon);

	/* background with an icon */
	Pixmap bg = x_create_default_pixmap(c, iconw, iconh);
	cairo_t *cr = create_cairo_for_pixmap(c, bg, iconw, iconh);

	cairo_set_source_rgba(cr, 0,0,0,1);
	cairo_paint(cr);
	blit_image(icon, cr, 0, 0);
	cairo_destroy(cr);

	XSetWindowAttributes attrs;
	attrs.background_pixmap = bg;
	attrs.override_redirect = True;

	/* the window */
	Window win = x_create_default_window(c, x, y, iconw, iconh, 
			CWOverrideRedirect | CWBackPixmap, &attrs);
	XFreePixmap(c->dpy, bg);

	/* create shape for a window */
	Pixmap mask = XCreatePixmap(c->dpy, c->root, iconw, iconh, 1);
	cr = create_cairo_for_bitmap(c, mask, iconw, iconh);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0,0,0,0);
	cairo_paint(cr);
	blit_image(icon, cr, 0, 0);
	cairo_destroy(cr);

	XShapeCombineMask(c->dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
	XFreePixmap(c->dpy, mask);

	return win;
}

static void client_msg(struct widget *w, XClientMessageEvent *e)
{
	struct panel *p = w->panel;
	struct x_connection *c = &p->connection;
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;

	if (e->message_type == c->atoms[XATOM_XDND_POSITION]) {
		int x = (e->data.l[2] >> 16) & 0xFFFF;
		int y = e->data.l[2] & 0xFFFF;

		/* if it's not ours, skip.. */
		if (   x >= (p->x + w->x) && x < (p->x + w->x + w->width)
			&& y >= (p->y + w->y) && y < (p->y + w->y + w->height) )
		{
			int ti = get_taskbar_task_at(w, x - p->x, y - p->y);
			if (ti != -1) {
				struct taskbar_task *t = &tw->tasks[ti];
				if (t->win != tw->active)
					activate_task(c, t);
			}

			x_send_dnd_message(c, e->data.l[0], 
					   c->atoms[XATOM_XDND_STATUS],
					   p->win,
					   2, /* bits: 0 1 */
					   0, 0,
					   None);
		}
	}
}

static void dnd_start(struct widget *w, struct drag_info *di)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct x_connection *c = &w->panel->connection;

	int ti = get_taskbar_task_at(di->taken_on, di->taken_x, di->taken_y);
	if (ti == -1)
		return;

	struct taskbar_task *t = &tw->tasks[ti];
	if (t->icon) {
		tw->dnd_win = create_window_for_dnd(c, 
						    di->cur_root_x, 
						    di->cur_root_y,
						    t->icon);
		XMapWindow(c->dpy, tw->dnd_win);
	}

	XDefineCursor(c->dpy, w->panel->win, tw->dnd_cur);
	tw->taken = t->win;
}

static void dnd_drag(struct widget *w, struct drag_info *di)
{
	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct x_connection *c = &w->panel->connection;
	if (tw->dnd_win != None)
		XMoveWindow(c->dpy, tw->dnd_win, di->cur_root_x, di->cur_root_y);
}

static void dnd_drop(struct widget *w, struct drag_info *di)
{
	/* ignore dragged data from other widgets */
	if (di->taken_on != w)
		return;

	struct taskbar_widget *tw = (struct taskbar_widget*)w->private;
	struct x_connection *c = &w->panel->connection;

	/* check if we have something draggable */
	if (tw->taken != None) {
		int taken = find_task_by_window(tw, tw->taken);
		int dropped = get_taskbar_task_at(w, di->dropped_x, di->dropped_y);
		if (di->taken_on == di->dropped_on && 
		    taken != -1 && dropped != -1 &&
		    tw->tasks[taken].desktop == tw->tasks[dropped].desktop)
		{
			/* if the desktop is the same.. move task */
			move_task(tw, taken, dropped);
			w->needs_expose = 1;
		} else if (!di->dropped_on && taken != -1) {
			/* out of the panel */
			if (di->cur_y < -tw->task_death_threshold || 
			    di->cur_y > w->height + tw->task_death_threshold ||
			    di->cur_x < -tw->task_death_threshold || 
			    di->cur_x > w->width + tw->task_death_threshold)
			{
				close_task(c, &tw->tasks[taken]);
			}
		}
	}

	XUndefineCursor(c->dpy, w->panel->win);

	if (tw->dnd_win != None) {
		XDestroyWindow(c->dpy, tw->dnd_win);
		tw->dnd_win = None;
	}
	tw->taken = None;
}
