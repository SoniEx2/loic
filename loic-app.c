/* Loic
 * Copyright (C) 2025 Soni L.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <gtk/gtk.h>

#include "loic-app.h"
#include "loic-app-window.h"

struct _LoicApp {
	GtkApplication parent;
};

G_DEFINE_TYPE(LoicApp, loic_app, GTK_TYPE_APPLICATION);

static void loic_app_init(LoicApp *app) {
}

static void quit_activated(
		GSimpleAction *action, GVariant *parameter, gpointer app) {
	g_application_quit(G_APPLICATION(app));
}

static GActionEntry app_entries[] = {
	{ "quit", quit_activated, NULL, NULL, NULL }
};

static void loic_app_startup(GApplication *app) {
	const char *quit_accels[2] = { "<Ctrl>Q", NULL };

	G_APPLICATION_CLASS(loic_app_parent_class)->startup(app);

	g_action_map_add_action_entries(
			G_ACTION_MAP(app), app_entries,
			G_N_ELEMENTS(app_entries), app);
	gtk_application_set_accels_for_action(
			GTK_APPLICATION(app), "app.quit", quit_accels);
}

static void loic_app_activate(GApplication *app) {
	LoicAppWindow *win;

	win = loic_app_window_new(LOIC_APP(app));
	gtk_window_present(GTK_WINDOW(win));
}

static void loic_app_class_init(LoicAppClass *class) {
	G_APPLICATION_CLASS(class)->startup = loic_app_startup;
	G_APPLICATION_CLASS(class)->activate = loic_app_activate;
}

LoicApp *loic_app_new(void) {
	return g_object_new(LOIC_APP_TYPE,
			"application-id", "space.autistic.loic",
			"flags", G_APPLICATION_DEFAULT_FLAGS,
			NULL);
}
