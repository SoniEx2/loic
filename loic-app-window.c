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

#include <string.h>
#include <stdbool.h>
#include <gtk/gtk.h>

#include "loic-app.h"
#include "loic-app-window.h"

#define streq(a, b) (strcmp(a, b) == 0)

struct IrcBuffer {
	gchar *name;
	GtkTextBuffer *buffer;
};

struct _LoicAppWindow {
	GtkApplicationWindow parent;

	/* template objects */
	GtkTextView *output_box;
	GtkWidget *input_box;
	GtkToggleButton *command_mode;
	GtkTextBuffer *status_buffer;
	GtkTextBuffer *help_text;

	/* managed objects */
	GCancellable *exiting;
	GSocketClient *client;
	GSocketConnection *libera;
	GSocketConnection *oftc;

	GSList *buffers;

	gulong handler_id;

	gchar *nick;
	gchar *libera_nick;
	gchar *oftc_nick;
};

static const char *const allowed_channels[] = {
	"#loic",
	NULL
};

G_DEFINE_TYPE(LoicAppWindow, loic_app_window, GTK_TYPE_APPLICATION_WINDOW);

#define MAX_BUFFER_LINES 1000

static void append_line_to_buffer(LoicAppWindow *win, GtkTextBuffer *buff,
		const char *line) {
	GtkTextIter start, end;
	gint lines;

	gtk_text_buffer_get_start_iter(buff, &start);
	gtk_text_buffer_get_end_iter(buff, &end);
	if (gtk_text_iter_compare(&start, &end) != 0) {
		gtk_text_buffer_insert(buff, &end, "\n", -1);
	}
	gtk_text_buffer_insert(buff, &end, line, -1);

	lines = gtk_text_buffer_get_line_count(buff);
	if (lines > MAX_BUFFER_LINES) {
		GtkTextIter line_iter;
		gtk_text_buffer_get_start_iter(buff, &start);
		gtk_text_buffer_get_iter_at_line(buff, &line_iter,
				lines - MAX_BUFFER_LINES);
		gtk_text_buffer_delete(buff, &start, &line_iter);
	}

	if (win->output_box &&
			gtk_text_view_get_buffer(win->output_box) == buff) {
		gtk_text_buffer_get_end_iter(buff, &end);
		gtk_text_view_scroll_to_iter(win->output_box, &end, 0.0, FALSE,
				0.0, 0.0);
	}
}

static void loic_app_window_append_line(LoicAppWindow *win, const char *line) {
	GtkTextBuffer *buff;

	if (!win->output_box) {
		return;
	}
	buff = gtk_text_view_get_buffer(win->output_box);
	if (buff == win->help_text) {
		buff = win->status_buffer;
	}
	append_line_to_buffer(win, buff, line);
}

static bool channeleq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		if (*a != *b) {
			if (*a >= 65 && *a <= 94) {
				if (*a + 32 != *b) {
					return false;
				}
			}
			if (*a >= 97 && *a <= 126) {
				if (*a - 32 != *b) {
					return false;
				}
			}
		}
	}
	return !*a && !*b;
}

static bool srceq(const char *src, const char *nick) {
	if (src[0] != ':') {
		return false;
	}
	src++;
	if (!strchr(src, '!')) {
		return false;
	}
	if ((strchr(src, '!') - src) != strlen(nick)) {
		return false;
	}
	for (; *src && *nick; src++, nick++) {
		if (*src != *nick) {
			if (*src >= 65 && *src <= 94) {
				if (*src + 32 != *nick) {
					return false;
				}
			}
			if (*src >= 97 && *src <= 126) {
				if (*src - 32 != *nick) {
					return false;
				}
			}
		}
	}
	return *src == '!' && !*nick;
}

static gint find_buffer_by_name(gconstpointer a, gconstpointer b) {
	const struct IrcBuffer *buf = a;
	const gchar *name = b;
	return !channeleq(buf->name, name);
}

static gint find_buffer_by_buffer(gconstpointer a, gconstpointer b) {
	const struct IrcBuffer *buf = a;
	const GtkTextBuffer *buff = b;
	return buf->buffer != buff;
}

#define LOIC_VERSION \
	"Loic - Low-effort IRC client" \
	" - https://github.com/SoniEx2/loic"

static void process_irc_message(GString *line, LoicAppWindow *win,
		GSocketConnection **conn) {
	gchar *last;
	gchar *c;
	gchar *prev;
	gchar **params;
	gint nparams;
	GStrvBuilder *params_builder;

	last = strstr(line->str, " :");

	if (last) {
		line->str[last - line->str] = '\0';
		last += 2;
	}

	params_builder = g_strv_builder_new();

	if (line->str[0] != ':') {
		g_strv_builder_add(params_builder, "");
	}

	prev = line->str;
	for (c = line->str; *c; c++) {
		for (; c[0] == ' ' && c[1] == ' '; c++) {
			c[0] = 0;
		}
		if (c[0] == ' ') {
			c[0] = 0;
			if (*prev) {
				g_strv_builder_add(params_builder, prev);
			}
			c++;
			prev = c;
			do {
				c[-1] = ' ';
				c--;
			} while (c > line->str && c[-1] == 0);
			c = prev - 1;
		}
	}
	if (*prev) {
		g_strv_builder_add(params_builder, prev);
	}
	if (last) {
		g_strv_builder_add(params_builder, last);
		last -= 2;
		line->str[last - line->str] = ' ';
	}
	
	params = g_strv_builder_end(params_builder);
	g_strv_builder_unref(params_builder);
	nparams = g_strv_length(params);

	if (nparams == 4 && streq(params[1], "PRIVMSG") && params[3][0] == 1) {
		if (streq(params[3], "\1VERSION\1") &&
				strchr(params[0], '!')) {
			GOutputStream *o_stream;
			gchar *src = g_strndup(params[0],
					strchr(params[0], '!') - params[0]);
			o_stream = g_io_stream_get_output_stream(
					G_IO_STREAM(*conn));
			g_output_stream_printf(o_stream, NULL, win->exiting,
					NULL,
					"NOTICE %s :\1VERSION %s\1\r\n",
					src + 1, LOIC_VERSION);
			g_free(src);
		}
	}
	if (nparams == 1 && params[0][0] == 0) {
		/* empty line, do nothing */
	} else if (nparams >= 2 && streq(params[1], "PING")) {
		/* respond with PONG */
		/* PING is weird so we need to do this for legacy reasons */
		GOutputStream *o_stream;
		char *token = line->str + strlen(params[0]);
		for (; *token == ' '; token++) {}
		token += 4;
		o_stream = g_io_stream_get_output_stream(G_IO_STREAM(*conn));
		g_output_stream_printf(o_stream, NULL, win->exiting, NULL,
				"PONG%s\r\n", token);
	} else if (nparams >= 3 && streq(params[1], "JOIN")) {
		gchar *channel = params[2];
		GSList *entry = g_slist_find_custom(win->buffers, channel,
				find_buffer_by_name);
		if (!entry) {
			struct IrcBuffer *buffer = g_new0(struct IrcBuffer, 1);
			buffer->name = g_strdup(channel);
			buffer->buffer = gtk_text_buffer_new(NULL);
			win->buffers = g_slist_prepend(win->buffers, buffer);
			entry = win->buffers;
		}
		append_line_to_buffer(win,
				((struct IrcBuffer *)entry->data)->buffer,
				line->str);
	} else if (nparams >= 4 && (streq(params[1], "PRIVMSG") || 
				streq(params[1], "NOTICE")) &&
			g_slist_find_custom(win->buffers, params[2],
				find_buffer_by_name)) {
		gchar *channel = params[2];
		GSList *entry = g_slist_find_custom(win->buffers, channel,
				find_buffer_by_name);
		append_line_to_buffer(win,
				((struct IrcBuffer *)entry->data)->buffer,
				line->str);
	} else if (nparams >= 3 && streq(params[1], "001")) {
		gchar *nick = g_strdup(params[2]);
		if (conn == &win->libera) {
			g_free(win->libera_nick);
			win->libera_nick = nick;
		} else if (conn == &win->oftc) {
			g_free(win->oftc_nick);
			win->oftc_nick = nick;
		} else {
			g_free(nick);
		}
		append_line_to_buffer(win, win->status_buffer, line->str);
	} else if (nparams >= 3 && streq(params[1], "NICK") &&
			conn == &win->libera && win->libera_nick &&
			srceq(params[0], win->libera_nick)) {
		g_free(win->libera_nick);
		win->libera_nick = g_strdup(params[2]);
		append_line_to_buffer(win, win->status_buffer, line->str);
	} else if (nparams >= 3 && streq(params[1], "NICK") &&
			conn == &win->oftc && win->oftc_nick &&
			srceq(params[0], win->oftc_nick)) {
		g_free(win->oftc_nick);
		win->oftc_nick = g_strdup(params[2]);
		append_line_to_buffer(win, win->status_buffer, line->str);
	} else {
		append_line_to_buffer(win, win->status_buffer, line->str);
	}

	g_strfreev(params);
	g_string_free(line, TRUE);
}

static void process_irc_messages(GBufferedInputStream *i_stream,
		LoicAppWindow *win, bool *skip, GSocketConnection **conn) {
	gsize count = 0;
	const guint8 *data = NULL;
	guint8 *nl = NULL;
	GString *line = NULL;
	gchar *nul = NULL;

	while ((data = g_buffered_input_stream_peek_buffer(i_stream, &count),
				nl = memchr(data, '\n', count)) != NULL) {
		gsize len = nl - data;
		if (nl != data && nl[-1] == '\r') {
			len--;
		}

		if (!*skip) {
			line = g_string_new_len((const gchar*)data, len);

			/* replace NULs */
			nul = line->str;
			while ((nul = memchr(nul, 0, len - (nul - line->str)))
					!= NULL) {
				/* "inert" replacement, not space or newline */
				*nul = '_';
			}
			g_string_replace(line, "\r", "_", 0);

			process_irc_message(line, win, conn);
			line = NULL;
		}
		*skip = false;

		g_input_stream_skip(G_INPUT_STREAM(i_stream), nl - data + 1,
				NULL, NULL);
	}

	if (count == g_buffered_input_stream_get_buffer_size(i_stream)) {
		/* too long for our buffer, truncate */
		if (!*skip) {
			line = g_string_new_len((const gchar*)data, count);

			/* replace NULs */
			nul = line->str;
			while ((nul = memchr(nul, 0, count - (nul - line->str)))
					!= NULL) {
				/* "inert" replacement, not space or newline */
				*nul = '_';
			}
			g_string_replace(line, "\r", "_", 0);

			process_irc_message(line, win, conn);
			line = NULL;
		}
		*skip = true;
		g_input_stream_skip(G_INPUT_STREAM(i_stream), count, NULL,
				NULL);
	}
}

static void libera_skip(GObject *i_stream, GAsyncResult *res, gpointer data);

static void libera_read(GObject *i_stream, GAsyncResult *res, gpointer data) {
	LoicAppWindow *win;
	GError *error = NULL;
	gssize nread;
	GBufferedInputStream *buf_i_stream = G_BUFFERED_INPUT_STREAM(i_stream);

	nread = g_buffered_input_stream_fill_finish(buf_i_stream, res, &error);
	if (nread > 0) {
		bool skip = false;

		win = LOIC_APP_WINDOW(data);

		process_irc_messages(buf_i_stream, win, &skip, &win->libera);

		if (skip) {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					libera_skip, win);
		} else {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					libera_read, win);
		}
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->libera);
	}
}

static void libera_skip(GObject *i_stream, GAsyncResult *res, gpointer data) {
	LoicAppWindow *win;
	GError *error = NULL;
	gssize nread;
	GBufferedInputStream *buf_i_stream = G_BUFFERED_INPUT_STREAM(i_stream);

	nread = g_buffered_input_stream_fill_finish(buf_i_stream, res, &error);
	if (nread > 0) {
		bool skip = true;

		win = LOIC_APP_WINDOW(data);

		process_irc_messages(buf_i_stream, win, &skip, &win->libera);

		if (skip) {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					libera_skip, win);
		} else {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					libera_read, win);
		}
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->libera);
	}
}

#define LIBERA_HOSTNAME "irc.libera.chat"
#define LIBERA_LOGIN \
	"NICK %s\r\n" \
	"USER loic-user 0 " LIBERA_HOSTNAME " :Loic User\r\n"

static void libera_connected(GObject *client, GAsyncResult *res,
		gpointer data) {
	LoicAppWindow *win;
	GSocketConnection *conn;
	GError *error = NULL;

	conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(client),
			res, &error);
	if (conn) {
		GInputStream *i_stream;
		GOutputStream *o_stream;
		GBufferedInputStream *buff_i_stream;

		win = LOIC_APP_WINDOW(data);

		g_clear_object(&win->libera);
		win->libera = conn;

		i_stream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
		buff_i_stream = G_BUFFERED_INPUT_STREAM(
				g_buffered_input_stream_new(i_stream));

		g_buffered_input_stream_fill_async(buff_i_stream, -1,
				G_PRIORITY_DEFAULT, win->exiting, libera_read,
				win);

		o_stream = g_io_stream_get_output_stream(G_IO_STREAM(conn));

		g_output_stream_printf(o_stream, NULL, win->exiting, NULL,
				LIBERA_LOGIN, win->nick ? win->nick : "");
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->libera);
	}
}

static void oftc_skip(GObject *i_stream, GAsyncResult *res, gpointer data);

static void oftc_read(GObject *i_stream, GAsyncResult *res, gpointer data) {
	LoicAppWindow *win;
	GError *error = NULL;
	gssize nread;
	GBufferedInputStream *buf_i_stream = G_BUFFERED_INPUT_STREAM(i_stream);

	nread = g_buffered_input_stream_fill_finish(buf_i_stream, res, &error);
	if (nread > 0) {
		bool skip = false;

		win = LOIC_APP_WINDOW(data);

		process_irc_messages(buf_i_stream, win, &skip, &win->oftc);

		if (skip) {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					oftc_skip, win);
		} else {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					oftc_read, win);
		}
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->oftc);
	}
}

static void oftc_skip(GObject *i_stream, GAsyncResult *res, gpointer data) {
	LoicAppWindow *win;
	GError *error = NULL;
	gssize nread;
	GBufferedInputStream *buf_i_stream = G_BUFFERED_INPUT_STREAM(i_stream);

	nread = g_buffered_input_stream_fill_finish(buf_i_stream, res, &error);
	if (nread > 0) {
		bool skip = true;

		win = LOIC_APP_WINDOW(data);

		process_irc_messages(buf_i_stream, win, &skip, &win->oftc);

		if (skip) {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					oftc_skip, win);
		} else {
			g_buffered_input_stream_fill_async(buf_i_stream, -1,
					G_PRIORITY_DEFAULT, win->exiting,
					oftc_read, win);
		}
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->oftc);
	}
}

#define OFTC_HOSTNAME "irc.oftc.net"
#define OFTC_LOGIN \
	"NICK %s\r\n" \
	"USER loic-user 0 " OFTC_HOSTNAME " :Loic User\r\n"

static void oftc_connected(GObject *client, GAsyncResult *res, gpointer data) {
	LoicAppWindow *win;
	GSocketConnection *conn;
	GError *error = NULL;

	conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(client),
			res, &error);
	if (conn) {
		GInputStream *i_stream;
		GOutputStream *o_stream;
		GBufferedInputStream *buff_i_stream;

		win = LOIC_APP_WINDOW(data);

		g_clear_object(&win->oftc);
		win->oftc = conn;

		i_stream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
		buff_i_stream = G_BUFFERED_INPUT_STREAM(
				g_buffered_input_stream_new(i_stream));

		g_buffered_input_stream_fill_async(buff_i_stream, -1,
				G_PRIORITY_DEFAULT, win->exiting, oftc_read,
				win);

		o_stream = g_io_stream_get_output_stream(G_IO_STREAM(conn));

		g_output_stream_printf(o_stream, NULL, win->exiting, NULL,
				OFTC_LOGIN, win->nick ? win->nick : "");
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		return;
	} else {
		win = LOIC_APP_WINDOW(data);
		g_clear_object(&win->oftc);
	}
}

static void loic_app_window_select_buffer(LoicAppWindow *win,
		GtkTextBuffer *buf) {
	if (win->output_box) {
		gtk_text_view_set_buffer(win->output_box, buf);
	}
}

static void connect_networks(LoicAppWindow *win) {
	if (win->libera == NULL) {
		g_socket_client_connect_to_host_async(win->client,
				LIBERA_HOSTNAME, 6697, win->exiting,
				libera_connected, win);
	}
	if (win->oftc == NULL) {
		g_socket_client_connect_to_host_async(win->client,
				OFTC_HOSTNAME, 6697, win->exiting,
				oftc_connected, win);
	}
}

static void activate_cb(GtkEntry *entry, gpointer user_data) {
	LoicAppWindow *win = LOIC_APP_WINDOW(user_data);
	GtkEntryBuffer *buff;
	const char *text;
	
	buff = gtk_entry_get_buffer(entry);
	text = gtk_entry_buffer_get_text(buff);

	if (win->command_mode &&
			gtk_toggle_button_get_active(win->command_mode)) {
		if (streq("help", text)) {
			loic_app_window_select_buffer(win, win->help_text);
		} else if (streq("status", text)) {
			loic_app_window_select_buffer(win, win->status_buffer);
		} else if (streq("nick", text)) {
			loic_app_window_append_line(win,
					"missing required argument: nick");
		} else if (g_str_has_prefix(text, "nick ")) {
			loic_app_window_append_line(win,
					"attempting to connect");
			win->nick = g_strdup(text + 5);
			connect_networks(win);
		} else if (streq("buffer", text)) {
			loic_app_window_append_line(win,
					"missing required argument: channel");
		} else if (g_str_has_prefix(text, "buffer ")) {
			GSList *entry = g_slist_find_custom(win->buffers,
					text + 7, find_buffer_by_name);
			if (entry) {
				struct IrcBuffer *buffer = entry->data;
				loic_app_window_select_buffer(win,
						buffer->buffer);
			} else {
				loic_app_window_append_line(win,
						"no such buffer");
			}
		} else if (streq("raw", text)) {
			loic_app_window_append_line(win,
					"missing required argument: network");
		} else if (streq("raw libera", text)) {
			loic_app_window_append_line(win,
					"missing required argument: line");
		} else if (streq("raw oftc", text)) {
			loic_app_window_append_line(win,
					"missing required argument: line");
		} else if (g_str_has_prefix(text, "raw libera ")) {
			GOutputStream *o_stream;
			if (win->libera) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->libera));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL, "%s\r\n",
						text + 11);
			}
		} else if (g_str_has_prefix(text, "raw oftc ")) {
			GOutputStream *o_stream;
			if (win->oftc) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->oftc));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL, "%s\r\n",
						text + 9);
			}
		} else if (g_str_has_prefix(text, "raw ")) {
			loic_app_window_append_line(win,
					"network must be libera or oftc");
		} else if (g_str_has_prefix(text, "join ")) {
			GOutputStream *o_stream;
			/* have to use this because we still wanna do further
			 * processing */
			bool flag = g_strv_contains(
					allowed_channels, text + 5);
			if (!flag) {
				loic_app_window_append_line(win,
						"channel does not allow loic");
			}
			if (flag && win->libera) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->libera));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL,
						"JOIN %s\r\n", text + 5);
			}
			if (flag && win->oftc) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->oftc));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL,
						"JOIN %s\r\n", text + 5);
			}
		} else {
			loic_app_window_append_line(win, "unknown command");
			return;
		}
		gtk_toggle_button_set_active(win->command_mode, FALSE);
	} else if (win->output_box) {
		GtkTextBuffer *textbuff =
			gtk_text_view_get_buffer(win->output_box);
		GSList *entry = g_slist_find_custom(win->buffers, textbuff,
				find_buffer_by_buffer);
		if (entry) {
			struct IrcBuffer *buffer = entry->data;
			gchar *sent_line;
			GOutputStream *o_stream;
			sent_line = g_strdup_printf("PRIVMSG %s :%s",
					buffer->name, text);
			append_line_to_buffer(win, textbuff, sent_line);
			if (win->libera) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->libera));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL, "%s\r\n",
						sent_line);
			}
			if (win->oftc) {
				o_stream = g_io_stream_get_output_stream(
						G_IO_STREAM(win->oftc));
				g_output_stream_printf(o_stream, NULL,
						win->exiting, NULL, "%s\r\n",
						sent_line);
			}
		}
	}
	gtk_entry_buffer_set_text(buff, "", 0);
}

static void loic_app_window_init(LoicAppWindow *win) {
	gtk_widget_init_template(GTK_WIDGET(win));

	win->exiting = g_cancellable_new();

	win->client = g_socket_client_new();
	g_socket_client_set_tls(win->client, TRUE);

	win->libera = NULL;
	win->oftc = NULL;

	win->nick = NULL;
	win->libera_nick = NULL;
	win->oftc_nick = NULL;

	win->buffers = NULL;
	
	if (win->input_box) {
		win->handler_id = g_signal_connect(win->input_box, "activate",
				G_CALLBACK(activate_cb),
				win);
	}
}

static void loic_app_window_toggle_command(GtkWidget *widget,
		const char *action_name, GVariant *parameter) {
	LoicAppWindow *win = LOIC_APP_WINDOW(widget);

	if (win->command_mode) {
		gboolean active;
		active = gtk_toggle_button_get_active(win->command_mode);
		gtk_toggle_button_set_active(win->command_mode, !active);
	}
}

static void free_buffers(gpointer data, gpointer user_data) {
	struct IrcBuffer *buff = data;
	g_clear_object(&buff->buffer);
	g_free(buff->name);
	g_free(data);
}

static void loic_app_window_dispose(GObject *gobject) {
	LoicAppWindow *win = LOIC_APP_WINDOW(gobject);
	GSList *slist;

	g_cancellable_cancel(win->exiting);
	g_clear_object(&win->exiting);
	g_clear_object(&win->client);
	g_clear_object(&win->libera);
	g_clear_object(&win->oftc);

	g_free(win->nick);
	win->nick = NULL;
	g_free(win->libera_nick);
	win->libera_nick = NULL;
	g_free(win->oftc_nick);
	win->oftc_nick = NULL;

	if (win->handler_id && win->input_box) {
		g_signal_handler_disconnect(win->input_box, win->handler_id);
		win->handler_id = 0;
	}

	slist = win->buffers;
	win->buffers = NULL;
	g_slist_foreach(slist, free_buffers, NULL);
	g_slist_free(slist);

	G_OBJECT_CLASS(loic_app_window_parent_class)->dispose(gobject);
}

static void loic_app_window_class_init(LoicAppWindowClass *class) {
	gtk_widget_class_set_template_from_resource(GTK_WIDGET_CLASS(class),
			"/space/autistic/loic/window.ui");

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class),
			LoicAppWindow, output_box);

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class),
			LoicAppWindow, input_box);

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class),
			LoicAppWindow, status_buffer);

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class),
			LoicAppWindow, help_text);

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class),
			LoicAppWindow, command_mode);

	gtk_widget_class_install_action(GTK_WIDGET_CLASS(class),
			"toggle_command", NULL,
			loic_app_window_toggle_command);

	gtk_widget_class_add_binding_action(GTK_WIDGET_CLASS(class),
			GDK_KEY_slash, GDK_CONTROL_MASK, "toggle_command",
			NULL);

	G_OBJECT_CLASS(class)->dispose = loic_app_window_dispose;
}

LoicAppWindow *loic_app_window_new(LoicApp *app) {
	return g_object_new(LOIC_APP_WINDOW_TYPE,
			"application", app,
			NULL);
}
