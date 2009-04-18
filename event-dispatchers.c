#include "gui.h"

static int point_in_rect(int px, int py, int x, int y, int w, int h)
{
	return (px > x &&
		px < x + w &&
		py > y &&
		py < y + h);
}

void disp_button_press_release(struct panel *p, XButtonEvent *e)
{
	if (e->type == ButtonRelease && !p->dnd.taken_on) {
		p->last_click_widget = 0;
		p->last_click_x = 0;
		p->last_click_y = 0;
	}
	int drag_status = -1;

	size_t i;
	for (i = 0; i < p->widgets_n; ++i) {
		struct widget *w = &p->widgets[i];
		if (point_in_rect(e->x, e->y, w->x, w->y, w->width, w->height)) {
			if (!p->dnd.taken_on) {
				if (e->type == ButtonPress) {
					p->last_click_widget = w;
					p->last_click_x = e->x;
					p->last_click_y = e->y;
				}
				if (w->interface->button_click)
					(*w->interface->button_click)(w, e);
			} else {
				if (e->type == ButtonRelease) {
					p->dnd.dropped_on = w;
					p->dnd.dropped_x = e->x;
					p->dnd.dropped_y = e->y;
					if (w->interface->dnd_drop)
						(*w->interface->dnd_drop)(&p->dnd);		
				}
			}
		}
	}
	if (e->type == ButtonRelease && p->dnd.taken_on) {
		struct widget *w = p->dnd.taken_on;
		if (w->interface->dnd_drop && p->dnd.taken_on != p->dnd.dropped_on)
			(*w->interface->dnd_drop)(&p->dnd);

		CLEAR_STRUCT(&p->dnd);
	}
}

void disp_motion_notify(struct panel *p, XMotionEvent *e)
{
	size_t i;

	/* is there any widget under mouse at all? for example if mouse is on
	   top of separator, there is no widget under it */
	int widget_under_mouse = 0;

	/* motion events: enter, leave, motion */
	for (i = 0; i < p->widgets_n; ++i) {
		struct widget *w = &p->widgets[i];
		if (point_in_rect(e->x, e->y, w->x, w->y, w->width, w->height)) {
			if (w == p->under_mouse) {
				if (w->interface->mouse_motion)
					(*w->interface->mouse_motion)(w, e);
			} else {
				if (p->under_mouse && 
					p->under_mouse->interface->mouse_leave)
				{
					(*p->under_mouse->interface->
						mouse_leave)(p->under_mouse);
				}
				p->under_mouse = w;
				if (w->interface->mouse_enter)
					(*w->interface->mouse_enter)(w);
			}
			widget_under_mouse = 1;
		}
	}
	if (!widget_under_mouse) {
		if (p->under_mouse && 
			p->under_mouse->interface->mouse_leave)
		{
			(*p->under_mouse->interface->mouse_leave)(p->under_mouse);
		}
		p->under_mouse = 0;
	}

	/* drag'n'drop moving */
	if (p->dnd.taken_on) {
		p->dnd.cur_x = e->x;
		p->dnd.cur_y = e->y;
		struct widget *w = p->dnd.taken_on;
		if (w->interface->dnd_drag)
			(*w->interface->dnd_drag)(&p->dnd);
	}

#define DRAG_THRESHOLD 5
	/* drag'n'drop detection */
	if (p->last_click_widget && (abs(p->last_click_x - e->x) > DRAG_THRESHOLD
			|| abs(p->last_click_y - e->y) > DRAG_THRESHOLD))
	{
		/* drag detected */
		struct widget *w = p->last_click_widget;
		p->dnd.taken_on = w;
		p->dnd.taken_x = p->last_click_x;
		p->dnd.taken_y = p->last_click_y;
		if (w->interface->dnd_start)
			(*w->interface->dnd_start)(&p->dnd);

		p->last_click_widget = 0;
		p->last_click_x = 0;
		p->last_click_y = 0;
	}
}

void disp_enter_leave_notify(struct panel *p, XCrossingEvent *e)
{
	if (e->type == LeaveNotify) {
		if (p->under_mouse && 
			p->under_mouse->interface->mouse_leave) 
		{
			(*p->under_mouse->interface->mouse_leave)(p->under_mouse);
		}
		p->under_mouse = 0;
	}
}

void disp_property_notify(struct panel *p, XPropertyEvent *e)
{
	size_t i;
	for (i = 0; i < p->widgets_n; ++i) {
		struct widget *w = &p->widgets[i];
		if (w->interface->prop_change)
			(*w->interface->prop_change)(w, e);
	}
}
