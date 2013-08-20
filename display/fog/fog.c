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
#include <string.h>

#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
#endif

#include "gol.h"
#include "compatibility.h"
#include "plugins/from_url.h"

#include "display_fog.xpm"

static GList* notifications;
static GList* popup_collections;

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
  gint default_timeout;
  gint timeout;
  gint offset;
  gboolean hover;
  struct {
    GtkWidget* popup;
    GtkWidget* title;
    GtkWidget* text;
  } widget;
} DISPLAY_INFO;

static inline void
free_display_info(DISPLAY_INFO* const di) {
  gtk_widget_destroy(di->widget.popup);
  memset(&di->widget, 0, sizeof(di->widget));
  free_notification_info(di->ni);
  g_free(di);
}

static gboolean
open_url(const gchar* url) {
#if defined(_WIN32)
  return (int) ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOW) > 32;
#elif defined(MACOSX)
  GError* error = NULL;
  const gchar *argv[] = {"open", url, NULL};
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
  di->ni->sticky = FALSE;
}

static void
display_enter(GtkWidget* GOL_UNUSED_ARG(widget), GdkEventMotion* GOL_UNUSED_ARG(event), gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = TRUE;
}

static void
display_leave(GtkWidget* GOL_UNUSED_ARG(widget), GdkEventMotion* GOL_UNUSED_ARG(event), gpointer user_data) {
  ((DISPLAY_INFO*) user_data)->hover = FALSE;
}

static inline DISPLAY_INFO*
reset_display_info(DISPLAY_INFO*, NOTIFICATION_INFO*);

static gboolean
display_animation_func(gpointer data) {
  DISPLAY_INFO* const di = (DISPLAY_INFO*) data;

  if (di->hover) return TRUE; // Do nothing.
  if (di->timeout >= 30 || !di->ni->sticky)
    --di->timeout;

  if (di->timeout < 0) {
    notifications = g_list_remove(notifications, di);
    popup_collections = g_list_append(popup_collections, di);
    reset_display_info(di, NULL);
    return FALSE;
  }

  if (di->offset < 160) {
    di->offset += 2;
    gdk_window_move_resize(di->widget.popup->window, di->x, di->y - di->offset, 180, di->offset);
  }

  if (di->timeout < 30) {
    gtk_window_set_opacity(GTK_WINDOW(di->widget.popup), (double) di->timeout/30.0*0.8);
  }
  return TRUE;
}

static void
label_size_allocate(GtkWidget* label, GtkAllocation* allocation, gpointer GOL_UNUSED_ARG(data)) {
  gtk_widget_set_size_request(label, allocation->width - 2, -1);
}

static inline GtkWidget*
get_container_nth_child(GtkContainer* const cont, const gint n) {
  if (!cont) return NULL;

  GtkWidget* widget = NULL;
  gint       cnt    = 0;
  void
  nth_getter(GtkWidget* const wid, gpointer GOL_UNUSED_ARG(user_data)) {
    if (cnt++ == n) widget = wid;
  }
  gtk_container_foreach(cont, nth_getter, NULL);
  return widget;
}

static inline GtkBox*
DISPLAY_HBOX(const DISPLAY_INFO* const di) {
  GtkWidget* const ebox = get_container_nth_child(GTK_CONTAINER(di->widget.popup), 0);
  if (!ebox) return NULL;

  GtkWidget* const vbox = get_container_nth_child(GTK_CONTAINER(ebox), 0);
  if (!vbox) return NULL;

  return GTK_BOX(get_container_nth_child(GTK_CONTAINER(vbox), 0));
}

static inline GtkWidget*
DISPLAY_HBOX_NTH_ELEM(const DISPLAY_INFO* const di, const gint n) {
  GtkBox* const hbox = DISPLAY_HBOX(di);
  if (!hbox) return NULL;

  return get_container_nth_child(GTK_CONTAINER(hbox), n);
}

static inline GtkImage*
DISPLAY_ICON_FIELD(const DISPLAY_INFO* const di) {
  return GTK_IMAGE(DISPLAY_HBOX_NTH_ELEM(di, 0));
}

static inline GtkLabel*
DISPLAY_TITLE_FIELD(const DISPLAY_INFO* const di) {
  return di ? GTK_LABEL(di->widget.title) : NULL;
}

static inline GtkLabel*
DISPLAY_TEXT_FIELD(const DISPLAY_INFO* const di) {
  return di ? GTK_LABEL(di->widget.text) : NULL;
}

static inline void
box_set_icon_if_has(const DISPLAY_INFO* const di) {
  if (!di) return;
  const NOTIFICATION_INFO* const ni = di->ni;
  if (!ni->icon || !*ni->icon) return;

  GdkPixbuf* const pixbuf =
    (ni->local ? pixbuf_from_url_as_file
               : pixbuf_from_url)(ni->icon, NULL);
  if (!pixbuf) return;

  GdkPixbuf* const tmp = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
  GtkWidget* const image = gtk_image_new_from_pixbuf(tmp ? tmp : pixbuf);
  if (image) {
    GtkBox* const hbox = DISPLAY_HBOX(di);
    gtk_box_pack_start(hbox, image, FALSE, FALSE, 0);
    gtk_box_reorder_child(hbox, DISPLAY_HBOX_NTH_ELEM(di, 0), 1);
  }

  if (tmp) g_object_unref(tmp);
  g_object_unref(pixbuf);
}

static inline void
remove_icon(const DISPLAY_INFO* const di) {
  if (!di) return;

  GtkBox* const hbox = DISPLAY_HBOX(di);
  GList* const children = gtk_container_get_children(GTK_CONTAINER(hbox));
  if (g_list_length(children) != 1) {
    GtkWidget* const image = g_list_nth_data(children, 0);
    gtk_box_reorder_child(hbox, image, -1);
    gtk_container_remove(GTK_CONTAINER(hbox), image);
  }
  g_list_free(children);
}

static inline DISPLAY_INFO*
create_popup_skelton() {
  DISPLAY_INFO* const di = g_new0(DISPLAY_INFO, 1);
  if (!di) return NULL;

  di->widget.popup = gtk_window_new(GTK_WINDOW_POPUP);
  if (!di->widget.popup) {
    free_display_info(di);
    return NULL;
  }
  gtk_window_set_title(GTK_WINDOW(di->widget.popup), "growl-for-linux");
  gtk_window_set_resizable(GTK_WINDOW(di->widget.popup), FALSE);
  gtk_window_set_decorated(GTK_WINDOW(di->widget.popup), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(di->widget.popup), TRUE);

  gtk_window_stick(GTK_WINDOW(di->widget.popup));
  gtk_widget_modify_bg(di->widget.popup, GTK_STATE_NORMAL, color_lightgray);

  GtkWidget* const ebox = gtk_event_box_new();
  if (!ebox) {
    free_display_info(di);
    return NULL;
  }
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
  g_signal_connect(G_OBJECT(ebox), "button-press-event", G_CALLBACK(display_clicked), di);
  g_signal_connect(G_OBJECT(ebox), "enter-notify-event", G_CALLBACK(display_enter), di);
  g_signal_connect(G_OBJECT(ebox), "leave-notify-event", G_CALLBACK(display_leave), di);
  gtk_container_add(GTK_CONTAINER(di->widget.popup), ebox);

  GtkWidget* const vbox = gtk_vbox_new(FALSE, 5);
  if (!vbox) {
    free_display_info(di);
    return NULL;
  }
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
  gtk_container_add(GTK_CONTAINER(ebox), vbox);

  GtkWidget* const hbox = gtk_hbox_new(FALSE, 5);
  if (!hbox) {
    free_display_info(di);
    return NULL;
  }
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

  di->widget.title = gtk_label_new(NULL);
  if (!di->widget.title) {
    free_display_info(di);
    return NULL;
  }
  gtk_widget_modify_fg(di->widget.title, GTK_STATE_NORMAL, color_black);
  gtk_widget_modify_font(di->widget.title, font_sans12_desc);
  gtk_box_pack_start(GTK_BOX(hbox), di->widget.title, FALSE, FALSE, 0);

  di->widget.text = gtk_label_new(NULL);
  if (!di->widget.text) {
    free_display_info(di);
    return NULL;
  }
  gtk_widget_modify_fg(di->widget.text, GTK_STATE_NORMAL, color_black);
  gtk_widget_modify_font(di->widget.text, font_sans8_desc);
  g_signal_connect(G_OBJECT(di->widget.text), "size-allocate", G_CALLBACK(label_size_allocate), NULL);
  gtk_label_set_line_wrap(GTK_LABEL(di->widget.text), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(di->widget.text), PANGO_WRAP_CHAR);
  gtk_box_pack_start(GTK_BOX(vbox), di->widget.text, FALSE, FALSE, 0);

  gtk_widget_set_size_request(di->widget.popup, 180, 1);

#ifdef _WIN32
  SetWindowPos(GDK_WINDOW_HWND(di->widget.popup->window), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
#endif

  return di;
}

static inline DISPLAY_INFO*
reset_display_info(DISPLAY_INFO* const di, NOTIFICATION_INFO* const ni) {
  if (ni) {
    di->default_timeout = ni->timeout;
    di->timeout = ni->timeout;
  } else {
    di->timeout = di->default_timeout;
  }
  di->pos     = 0;
  di->offset  = 0;
  di->hover   = FALSE;
  free_notification_info(di->ni);
  di->ni = ni;
  gtk_widget_hide_all(di->widget.popup);
  gtk_window_set_opacity(GTK_WINDOW(di->widget.popup), 0.8);
  remove_icon(di);
  return di;
}

static inline gpointer
list_pop_front(GList** list) {
  if (!list) return NULL;
  const gpointer elem = g_list_nth_data(*list, 0);
  *list = g_list_remove(*list, elem);
  return elem;
}

static inline DISPLAY_INFO*
get_popup_skelton(NOTIFICATION_INFO* const ni) {
  DISPLAY_INFO* const di = (DISPLAY_INFO*) list_pop_front(&popup_collections);
  if (di) {
    di->default_timeout = ni->timeout;
    di->timeout = ni->timeout;
    di->ni = ni;
    return di;
  }
  return reset_display_info(create_popup_skelton(), ni);
}

G_MODULE_EXPORT gboolean
display_show(NOTIFICATION_INFO* const ni) {
  DISPLAY_INFO* const di = get_popup_skelton(ni);
  if (!di) return FALSE;

  gint
  is_differ_pos(gconstpointer p, gconstpointer GOL_UNUSED_ARG(user_data)) {
    return ((const DISPLAY_INFO*) p)->pos == di->pos++;
  }
  GList* const found = g_list_find_custom(notifications, NULL, is_differ_pos);
  if (found) --di->pos;

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

  box_set_icon_if_has(di);
  gtk_label_set_text(DISPLAY_TITLE_FIELD(di), di->ni->title);
  gtk_label_set_text(DISPLAY_TEXT_FIELD(di), di->ni->text);

  gtk_window_move(GTK_WINDOW(di->widget.popup), di->x, di->y);
  gtk_widget_show_all(di->widget.popup);
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

  void
  list_free_deep(GList* const list) {
    void
    deleter_wrapper(gpointer data, gpointer GOL_UNUSED_ARG(user_data)) {
      free_display_info((DISPLAY_INFO*) data);
    }
    g_list_foreach(list, deleter_wrapper, NULL);
    g_list_free(list);
  }
  list_free_deep(notifications);
  list_free_deep(popup_collections);

  // FIXME: g_list_free_full will fail symbol lookup.
  //void
  //deleter(gpointer data) {
  //  free_display_info((DISPLAY_INFO*) data);
  //}
  //g_list_free_full(notifications, deleter);
  //g_list_free_full(popup_collections, deleter);
}

G_MODULE_EXPORT const gchar*
display_name() {
  return "Fog";
}

G_MODULE_EXPORT const gchar*
display_description() {
  return
    "<span size=\"large\"><b>Fog</b></span>\n"
    "<span>This is fog notification display.</span>\n"
    "<span>Slide-up white box. And fadeout after a while.</span>\n";
}

G_MODULE_EXPORT char**
display_thumbnail() {
  return display_fog;
}

// vim:set et sw=2 ts=2 ai:
