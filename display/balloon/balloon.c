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
#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
#endif
#include <ctype.h>
#include <stdlib.h>
#include <memory.h>
#include <curl/curl.h>
#include "../../gol.h"
#include "../../plugins/from_url.h"
#include "balloon.xpm"
#include "display_balloon.xpm"

static gchar* param = NULL;
static GList* notifications = NULL;
static GdkPixmap* pixmap = NULL;
static GdkBitmap* bitmap = NULL;
static gint pixmap_width, pixmap_height;

static GdkColor inst_color_white_;
static const GdkColor* const color_white = &inst_color_white_;

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

static void
free_display_info(DISPLAY_INFO* di) {
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
display_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  DISPLAY_INFO* di = (DISPLAY_INFO*) user_data;
  if (di->timeout >= 30) di->timeout = 30;
  if (di->ni->url && *di->ni->url) open_url(di->ni->url);
}

static void
display_enter(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = TRUE;
}

static void
display_leave(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = FALSE;
}

static gboolean
display_animation_func(gpointer data) {
  DISPLAY_INFO* di = (DISPLAY_INFO*) data;

  if (!di->hover) di->timeout--;

  if (di->timeout < 0) {
    gtk_widget_destroy(di->popup);
    notifications = g_list_remove(notifications, di);
    free_display_info(di);
    return FALSE;
  }

  if (di->timeout > 450) {
    gtk_window_set_opacity(GTK_WINDOW(di->popup), (double) (500-di->timeout)/50.0*0.8);
  }

  if (di->timeout < 50) {
    gtk_window_set_opacity(GTK_WINDOW(di->popup), (double) di->timeout/50.0*0.8);
  }
  return TRUE;
}

static gboolean
display_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
  gdk_window_clear_area(
    widget->window,
    event->area.x, event->area.y, event->area.width, event->area.height);
  gdk_draw_pixmap(
    widget->window,
    widget->style->fg_gc[GTK_STATE_NORMAL],
    pixmap,
    0, 0,
    0, 0, pixmap_width, pixmap_height);
  return FALSE;
}

static GList*
find_showable_position() {
  gint pos = 0;
  gint
  is_differ_pos(gconstpointer p, gconstpointer unused_) {
    return ((const DISPLAY_INFO*) p)->pos == pos++;
  }
  return  g_list_find_custom(notifications, NULL, is_differ_pos);
}

static void
label_size_allocate(GtkWidget* label, GtkAllocation* allocation, gpointer data) {
  gtk_widget_set_size_request(label, allocation->width - 2, -1);
}

G_MODULE_EXPORT gboolean
display_show(NOTIFICATION_INFO* ni) {
  DISPLAY_INFO* di = g_new0(DISPLAY_INFO, 1);
  if (!di) {
    perror("g_new0");
    return FALSE;
  }
  di->ni = ni;

  GList* const found = find_showable_position();
  di->pos = found ? g_list_position(notifications, found) : (gint) g_list_length(notifications);

  const gint vert_count = screen_rect.height / 110;
  const gint cx = di->pos / vert_count;
  const gint cy = di->pos % vert_count;
  di->x = screen_rect.x + screen_rect.width  - (cx + 1) * 250;
  di->y = screen_rect.y + screen_rect.height - (cy + 1) * 110;
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

  GtkWidget* ebox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
  gtk_container_add(GTK_CONTAINER(di->popup), ebox);

  GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 18);
  gtk_container_add(GTK_CONTAINER(ebox), vbox);

  GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

  if (di->ni->icon && *di->ni->icon) {
    GdkPixbuf* pixbuf;
    if (di->ni->local) {
      gchar* newurl = g_filename_from_uri(di->ni->icon, NULL, NULL);
      GError* error = NULL;
      pixbuf = gdk_pixbuf_new_from_file(newurl ? newurl : di->ni->icon, &error);
      g_free(newurl);
    } else {
      pixbuf = pixbuf_from_url(di->ni->icon, NULL);
    }

    if (pixbuf) {
      GdkPixbuf* tmp = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
      if (tmp) {
        g_object_unref(pixbuf);
        pixbuf = tmp;
      }
      GtkWidget* image = gtk_image_new_from_pixbuf(pixbuf);
      gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
      g_object_unref(pixbuf);
    }
  }

  GtkWidget* label = gtk_label_new(di->ni->title);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, color_white);
  gtk_widget_modify_font(label, font_sans12_desc);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  label = gtk_label_new(di->ni->text);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, color_white);
  gtk_widget_modify_font(label, font_sans8_desc);
  g_signal_connect(G_OBJECT(label), "size-allocate", G_CALLBACK(label_size_allocate), NULL);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, FALSE, 0);

  g_signal_connect(G_OBJECT(ebox), "button-press-event", G_CALLBACK(display_clicked), di);
  g_signal_connect(G_OBJECT(ebox), "enter-notify-event", G_CALLBACK(display_enter), di);
  g_signal_connect(G_OBJECT(ebox), "leave-notify-event", G_CALLBACK(display_leave), di);

  di->offset = 0;
  di->timeout = 500;

  gtk_window_move(GTK_WINDOW(di->popup), di->x, di->y);
  gtk_window_set_opacity(GTK_WINDOW(di->popup), 0);
  gtk_widget_set_app_paintable(di->popup, TRUE);
  gtk_widget_show_all(di->popup);

  if (pixmap == NULL) {
     pixmap = gdk_pixmap_create_from_xpm_d(di->popup->window, &bitmap, NULL, balloon);
  }
  gdk_drawable_get_size(pixmap, &pixmap_width, &pixmap_height);
  gtk_widget_set_size_request(di->popup, pixmap_width, pixmap_height);
  gdk_window_shape_combine_mask(di->popup->window, bitmap, 0, 0);
  g_signal_connect(G_OBJECT(di->popup), "expose-event", G_CALLBACK(display_expose), di);

  g_timeout_add(10, display_animation_func, di);

  return FALSE;
}

G_MODULE_EXPORT gboolean
display_init() {
  gdk_color_parse("white", &inst_color_white_);

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
  return "Balloon";
}

G_MODULE_EXPORT const gchar*
display_description() {
  return
    "<span size=\"large\"><b>Balloon</b></span>\n"
    "<span>This is balloon notification display.</span>\n"
    "<span>Fade-in black box. And fadeout after a while.</span>\n";
}

G_MODULE_EXPORT char**
display_thumbnail() {
  return display_balloon;
}

G_MODULE_EXPORT char*
display_get_param() {
  return param;
}

G_MODULE_EXPORT void
display_set_param(const gchar* p) {
  if (param) g_free(param);
  param = g_strdup(p);
}

// vim:set et sw=2 ts=2 ai:
