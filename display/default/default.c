/* Copyright 2011 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
#endif
#include <curl/curl.h>
#include "gol.h"
#include "plugins/from_url.h"
#include "display_default.xpm"

static GList* notifications = NULL;

static GdkColor inst_color_lightgray_;
static GdkColor inst_color_black_;
static const GdkColor* const color_lightgray = &inst_color_lightgray_;
static const GdkColor* const color_black     = &inst_color_black_;

static PangoFontDescription* font_sans12_desc;
static PangoFontDescription* font_sans8_desc;

static GdkRectangle screen_rect;

typedef struct {
  NOTIFICATION_INFO* ni;
  gint pos;
  gint x, y;
  gint timeout;
  GtkWidget* popup;
  gint offset;
  gboolean sticky;
  gboolean hover;
} DISPLAY_INFO;

static inline void
free_display_info(DISPLAY_INFO* const di) {
  g_free(di->ni->title);
  g_free(di->ni->text);
  g_free(di->ni->icon);
  g_free(di->ni->url);
  g_free(di->ni);
  g_free(di);
}

static gboolean
open_url(const gchar* url) {
#if defined(_WIN32)
  return (int) ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOW) > 32;
#elif defined(MACOSX)
  GError* error = NULL;
  const gchar *argv[] = {"open", (gchar*) url, NULL};
  return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
#else
  GError* error = NULL;
  gchar *argv[] = {"xdg-open", (gchar*) url, NULL};
  return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
#endif
}

static void
display_clicked(GtkWidget* GOL_UNUSED_ARG(widget), GdkEvent* GOL_UNUSED_ARG(event), gpointer user_data) {
  DISPLAY_INFO* const di = (DISPLAY_INFO*) user_data;
  if (di->timeout >= 30) di->timeout = 30;
  if (di->ni->url && *di->ni->url) open_url(di->ni->url);
}

static void
display_enter(GtkWidget* GOL_UNUSED_ARG(widget), GdkEventMotion* GOL_UNUSED_ARG(event), gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = TRUE;
}

static void
display_leave(GtkWidget* GOL_UNUSED_ARG(widget), GdkEventMotion* GOL_UNUSED_ARG(event), gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = FALSE;
}

static gboolean
display_animation_func(gpointer data) {
  DISPLAY_INFO* const di = (DISPLAY_INFO*) data;

  if (!di->hover) di->timeout--;

  if (di->timeout < 0) {
    gtk_widget_destroy(di->popup);
    notifications = g_list_remove(notifications, di);
    free_display_info(di);
    return FALSE;
  }

  if (di->offset < 160) {
    di->offset += 2;
    gdk_window_move_resize(di->popup->window, di->x, di->y - di->offset, 180, di->offset);
  }

  if (di->timeout < 30) {
    gtk_window_set_opacity(GTK_WINDOW(di->popup), (double) di->timeout/30.0*0.8);
  }
  return TRUE;
}

static GList*
find_showable_position() {
  gint pos = 0;
  gint
  is_differ_pos(gconstpointer p, gconstpointer GOL_UNUSED_ARG(user_data)) {
    return ((const DISPLAY_INFO*) p)->pos == pos++;
  }
  return g_list_find_custom(notifications, NULL, is_differ_pos);
}

static void
label_size_allocate(GtkWidget* label, GtkAllocation* allocation, gpointer GOL_UNUSED_ARG(data)) {
  gtk_widget_set_size_request(label, allocation->width - 2, -1);
}

static inline DISPLAY_INFO*
create_display_info_with_notification_info(NOTIFICATION_INFO* const ni) {
  if (!ni) return NULL;

  DISPLAY_INFO* const di = g_new0(DISPLAY_INFO, 1);
  if (!di) {
    perror("g_new0");
    return NULL;
  }
  di->ni = ni;
  return di;
}

static inline void
box_enable_icon(GtkBox* const restrict box, const NOTIFICATION_INFO* const restrict ni) {
  GdkPixbuf* const pixbuf =
    (ni->local ? pixbuf_from_url_as_file
               : pixbuf_from_url)(ni->icon, NULL);
  if (!pixbuf) return;

  GdkPixbuf* const tmp   = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
  GtkWidget* const image = gtk_image_new_from_pixbuf(tmp ? tmp : pixbuf);
  gtk_box_pack_start(box, image, FALSE, FALSE, 0);

  g_object_unref(tmp);
  g_object_unref(pixbuf);
}

G_MODULE_EXPORT gboolean
display_show(NOTIFICATION_INFO* const ni) {
  DISPLAY_INFO* const di = create_display_info_with_notification_info(ni);
  if (!di) return FALSE;

  GList* const found = find_showable_position();
  di->pos = found ? g_list_position(notifications, found) : (gint) g_list_length(notifications);

  const gint vert_count = screen_rect.height / 180;
  const gint cx = di->pos / vert_count;
  const gint cy = di->pos % vert_count;
  di->x = screen_rect.x + screen_rect.width  - cx * 200 - 180;
  di->y = screen_rect.y + screen_rect.height - cy * 180 + 20;
  if (di->x < 0) {
    free_display_info(di);
    return FALSE;
  }
  notifications = g_list_insert_before(notifications, found, di);

  di->popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_title(GTK_WINDOW(di->popup), "growl-for-linux");
  gtk_window_set_resizable(GTK_WINDOW(di->popup), FALSE);
  gtk_window_set_decorated(GTK_WINDOW(di->popup), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(di->popup), TRUE);

  gtk_window_stick(GTK_WINDOW(di->popup));
  gtk_window_set_opacity(GTK_WINDOW(di->popup), 0.8);
  gtk_widget_modify_bg(di->popup, GTK_STATE_NORMAL, color_lightgray);

  GtkWidget* const ebox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
  gtk_container_add(GTK_CONTAINER(di->popup), ebox);

  GtkWidget* const vbox = gtk_vbox_new(FALSE, 5);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
  gtk_container_add(GTK_CONTAINER(ebox), vbox);

  GtkWidget* const hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

  if (di->ni->icon && *di->ni->icon) box_enable_icon(GTK_BOX(hbox), di->ni);

  GtkWidget* label = gtk_label_new(di->ni->title);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, color_black);
  gtk_widget_modify_font(label, font_sans12_desc);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  label = gtk_label_new(di->ni->text);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, color_black);
  gtk_widget_modify_font(label, font_sans8_desc);
  g_signal_connect(G_OBJECT(label), "size-allocate", G_CALLBACK(label_size_allocate), NULL);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  gtk_widget_set_size_request(di->popup, 180, 1);

  g_signal_connect(G_OBJECT(ebox), "button-press-event", G_CALLBACK(display_clicked), di);
  g_signal_connect(G_OBJECT(ebox), "enter-notify-event", G_CALLBACK(display_enter), di);
  g_signal_connect(G_OBJECT(ebox), "leave-notify-event", G_CALLBACK(display_leave), di);

  di->offset = 0;
  di->timeout = 500;

  gtk_window_move(GTK_WINDOW(di->popup), di->x, di->y);
  gtk_widget_show_all(di->popup);

#ifdef _WIN32
  SetWindowPos(GDK_WINDOW_HWND(di->popup->window), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
#endif

  g_timeout_add(10, display_animation_func, di);

  return FALSE;
}

G_MODULE_EXPORT gboolean
display_init() {
  gdk_color_parse("lightgray", &inst_color_lightgray_);
  gdk_color_parse("black", &inst_color_black_);

  font_sans12_desc = pango_font_description_new();
  pango_font_description_set_family(font_sans12_desc, "Sans");
  pango_font_description_set_size(font_sans12_desc, 12 * PANGO_SCALE);

  font_sans8_desc = pango_font_description_new();
  pango_font_description_set_family(font_sans8_desc, "Sans");
  pango_font_description_set_size(font_sans8_desc, 8 * PANGO_SCALE);

  GdkScreen* const screen = gdk_screen_get_default();
  const gint monitor_num = gdk_screen_get_primary_monitor(screen);
  gdk_screen_get_monitor_geometry(screen, monitor_num, &screen_rect);

  return TRUE;
}

G_MODULE_EXPORT void
display_term() {
  pango_font_description_free(font_sans12_desc);
  pango_font_description_free(font_sans8_desc);
}

G_MODULE_EXPORT const gchar*
display_name() {
  return "Default";
}

G_MODULE_EXPORT const gchar*
display_description() {
  return
    "<span size=\"large\"><b>Default</b></span>\n"
    "<span>This is default notification display.</span>\n"
    "<span>Slide-up white box. And fadeout after a while.</span>\n";
}

G_MODULE_EXPORT char**
display_thumbnail() {
  return display_default;
}

// vim:set et sw=2 ts=2 ai:
