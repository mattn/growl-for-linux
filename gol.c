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
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <netinet/tcp.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <unistd.h>
#endif
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <sqlite3.h>
#ifdef _WIN32
# include <io.h>
#endif
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/des.h>
#include "gol.h"

#ifdef _WIN32
typedef char sockopt_t;
typedef int socklen_t;
# ifndef snprintf
#  define snprintf _snprintf
# endif
# ifndef strncasecmp
#  define strncasecmp(d,s,n) strnicmp(d,s,n)
# endif
# ifndef srandom
#  define srandom(s) srand(s)
# endif
# ifndef random
#  define random() rand()
# endif
#else
# define closesocket(x) close(x)
typedef int sockopt_t;
# ifndef SD_BOTH
#  define SD_BOTH SHUT_RDWR
# endif
#endif

#define GOL_PP_JOIN(_left, _right) _left ## _right

#define GNTP_OK_STRING_LITERAL(_version, _action)   \
  "GNTP/" _version " -OK NONE\r\n"                  \
  "Response-Action: " _action "\r\n\r\n"            \

#define GNTP_ERROR_STRING_LITERAL(_version, _message, _desc)  \
  "GNTP/" _version " -ERROR " _message "\r\n"                 \
  "Error-Description: " _desc "\r\n\r\n"                      \

typedef struct {
  void* handle;
  gboolean (*init)();
  gboolean (*start)();
  gboolean (*stop)();
  gboolean (*term)();
  gchar* (*name)();
  gchar* (*description)();
  gchar** (*thumbnail)();
} SUBSCRIBE_PLUGIN;

typedef struct {
  void* handle;
  gboolean (*init)();
  gboolean (*show)(NOTIFICATION_INFO* ni);
  gboolean (*term)();
  gchar* (*name)();
  gchar* (*description)();
  gchar** (*thumbnail)();
} DISPLAY_PLUGIN;

static gchar* password = NULL;
static gboolean require_password_for_local_apps = FALSE;
static gboolean require_password_for_lan_apps = FALSE;
static sqlite3 *db = NULL;
static GtkStatusIcon* status_icon = NULL;
static GtkWidget* popup_menu = NULL;
static GtkWidget* setting_dialog = NULL;
static GtkWidget* about_dialog = NULL;
static GList* display_plugins = NULL;
static GList* subscribe_plugins = NULL;
static DISPLAY_PLUGIN* current_display = NULL;
static gchar* exepath = NULL;
static SUBSCRIPTOR_CONTEXT sc;

#ifndef LIBDIR
# define LIBDIR exepath
#endif
#ifndef DATADIR
# define DATADIR exepath
#endif

static const char*
skipsp(const char* str)
{
  for (; isspace(*str); ++str);
  return str;
}

static void*
safely_realloc(void* ptr, const size_t newsize)
{
  void* const tmp = realloc(ptr, newsize);
  if (!tmp) {
    perror("realloc");
    free(ptr);
    return NULL;
  }
  return tmp;
}

static size_t
read_all(int fd, char** ptr) {
  const struct timeval timeout =
  {
    .tv_sec  = 1,
    .tv_usec = 0,
  };

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  const size_t len = BUFSIZ;
  size_t bufferlen = len;
  char* buf = (char*) malloc(bufferlen + 1);
  if (!buf) {
    perror("malloc");
    return 0;
  }

  char* end = buf;
  ptrdiff_t datalen = 0;
  for (ssize_t r; (r = recv(fd, end, bufferlen - datalen, 0)) >= 0; ) {
    if (r == 0) continue;
    *(end += r) = '\0';
    datalen += r;
    if (r >= 4 && !strncmp(end - 4, "\r\n\r\n", 4)) break;

    bufferlen += len;
    buf = (char*) safely_realloc(buf, bufferlen + 1);
    if (!buf) return 0;
    end = buf + datalen;
  }
  *ptr = buf;
  return (ptrdiff_t) (end - buf);
}

unsigned int
unhex(unsigned char c) {
  if (!isxdigit(c)) return 0;
  const char str[] = {c, '\0'};
  char* _unused_endptr;
  return strtol(str, &_unused_endptr, 16);
}

DISPLAY_PLUGIN*
find_display_plugin_or(bool(* pred)(const DISPLAY_PLUGIN*), DISPLAY_PLUGIN* const or_dp) {
  gint
  wrapped_pred(gconstpointer dp, gconstpointer _unused) {
    return pred((const DISPLAY_PLUGIN*) dp) ? 0 : 1;
  }
  GList* elem = g_list_find_custom(display_plugins, NULL, wrapped_pred);
  return elem ? (DISPLAY_PLUGIN*) elem->data : or_dp;
}

DISPLAY_PLUGIN*
find_display_plugin(bool(* pred)(const DISPLAY_PLUGIN*)) {
  return find_display_plugin_or(pred, NULL);
}

SUBSCRIBE_PLUGIN*
find_subscribe_plugin(bool(* pred)(const SUBSCRIBE_PLUGIN*)) {
  gint
  wrapped_pred(gconstpointer sp, gconstpointer _unused) {
    return pred((const SUBSCRIBE_PLUGIN*) sp) ? 0 : 1;
  }
  GList* elem = g_list_find_custom(subscribe_plugins, NULL, wrapped_pred);
  return elem ? (SUBSCRIBE_PLUGIN*) elem->data : NULL;
}

void
foreach_display_plugin(void(* func)(DISPLAY_PLUGIN*)) {
  void
  wrapped_func(gpointer dp, gpointer _unused) {
    func((DISPLAY_PLUGIN*) dp);
  }
  g_list_foreach(display_plugins, wrapped_func, NULL);
}

void
foreach_subscribe_plugin(void(* func)(SUBSCRIBE_PLUGIN*)) {
  void
  wrapped_func(gpointer sp, gpointer _unused) {
    func((SUBSCRIBE_PLUGIN*) sp);
  }
  g_list_foreach(subscribe_plugins, wrapped_func, NULL);
}

static inline void
exec_splite3(const char *, ...);

static void
exec_splite3(const char * const tsql, ...)
{
  if (tsql) {
    va_list list;
    va_start(list, tsql);

    char* const sql = sqlite3_vmprintf(tsql, list);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    va_end(list);
  }
}

static gboolean
get_config_bool(const char* key, gboolean def) {
  char* const sql = sqlite3_mprintf(
      "select value from config where key = '%q'", key);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  const gboolean ret =
    sqlite3_step(stmt) == SQLITE_ROW
      ? (gboolean) sqlite3_column_int(stmt, 0)
      : def;
  sqlite3_finalize(stmt);
  sqlite3_free(sql);
  return ret;
}

static gboolean
get_subscriber_enabled(const char* name) {
  char* const sql = sqlite3_mprintf(
      "select enable from subscriber where name = '%q'", name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  const gboolean ret =
    sqlite3_step(stmt) == SQLITE_ROW
      ? (gboolean) sqlite3_column_int(stmt, 0)
      : FALSE;
  sqlite3_finalize(stmt);
  sqlite3_free(sql);
  return ret;
}

static gchar*
get_config_string(const char* const key, const char* const def) {
  char* const sql = sqlite3_mprintf(
      "select value from config where key = '%q'", key);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  gchar* const value =
    sqlite3_step(stmt) == SQLITE_ROW
      ? g_strdup((char*) sqlite3_column_text(stmt, 0))
      : g_strdup(def ? def : "");
  sqlite3_finalize(stmt);
  sqlite3_free(sql);
  return value;
}

static void
set_config_bool(const char* key, gboolean value) {
  exec_splite3("delete from config where key = '%q'", key);

  exec_splite3(
    "insert into config(key, value) values('%q', '%q')",
    key, value ? "1" : "0");
}

static void
set_config_string(const char* const key, const char* const value) {
  exec_splite3("delete from config where key = '%q'", key);

  exec_splite3("insert into config(key, value) values('%q', '%q')", key, value);
}

static void
my_gtk_status_icon_position_menu(
    GtkMenu* const menu, gint* const x, gint* const y,
    gboolean* const push_in, const gpointer user_data) {
  gtk_status_icon_position_menu(menu, x, y, push_in, user_data);
#ifdef _WIN32
  RECT rect;
  SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);
  const gint h = GTK_WIDGET(menu)->requisition.height;
  if (*y + h > rect.bottom) *y -= h;
#endif
}

static void
status_icon_popup(
    GtkStatusIcon* const status_icon, const guint button,
    const guint32 activate_time, const gpointer menu) {
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
      my_gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static bool
get_tree_model_from_selection(gchar** const restrict pname, GtkTreeSelection* const restrict selection) {
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return false;

  gtk_tree_model_get(model, &iter, 0, pname, -1);
  return true;
}

static bool
get_tree_model_from_tree(gchar** const restrict pname, GtkWidget* const restrict tree) {
  GtkTreeSelection* const selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  return get_tree_model_from_selection(pname, selection);
}

static GtkTreeIter
list_store_set_after_append(GtkListStore* const list_store, ...) {
  va_list list;
  va_start(list, list_store);
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set_valist(list_store, &iter, list);
  va_end(list);
  return iter;
}

static void
display_tree_selection_changed(GtkTreeSelection * const selection, const gpointer user_data) {
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const cp = find_display_plugin_or(is_selection_name, current_display);

  GtkWidget* const label =
    (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "description");
  gtk_label_set_markup(GTK_LABEL(label), "");
  if (cp->description) {
    gtk_label_set_markup(GTK_LABEL(label), cp->description());
  }

  GtkWidget* const image =
    (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "thumbnail");
  gtk_image_clear(GTK_IMAGE(image));
  if (cp->thumbnail) {
    GdkBitmap* bitmap;
    GdkPixmap* pixmap = gdk_pixmap_colormap_create_from_xpm_d(
        NULL, gdk_colormap_get_system(), &bitmap, NULL, cp->thumbnail());
    gtk_image_set_from_pixmap(GTK_IMAGE(image), pixmap, bitmap);
    gdk_pixmap_unref(pixmap);
    gdk_bitmap_unref(bitmap);
  }
  g_free(name);
}

static void
application_tree_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
  gchar* app_name;
  if (!get_tree_model_from_selection(&app_name, selection)) return;

  GtkListStore* model2 =
    (GtkListStore*) g_object_get_data(G_OBJECT(user_data), "model2");
  gtk_list_store_clear(model2);
  char* const sql = sqlite3_mprintf(
      "select distinct name from notification"
      " where app_name = '%q' order by name", app_name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    list_store_set_after_append(
        GTK_LIST_STORE(model2), 0, sqlite3_column_text(stmt, 0), -1);
  }
  sqlite3_finalize(stmt);
  sqlite3_free(sql);

  g_free(app_name);

  gtk_widget_set_sensitive(
    (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "enable"), FALSE);
  gtk_widget_set_sensitive(
    (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "display"), FALSE);
}

static void
set_as_default_clicked(GtkWidget* widget, gpointer user_data) {
  GtkTreeSelection* const selection = (GtkTreeSelection*) user_data;
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const dp = find_display_plugin(is_selection_name);
  if (dp) {
    current_display = dp;
    set_config_string("default_display", name);
  }
  g_free(name);
}

static void
preview_clicked(GtkWidget* widget, gpointer user_data) {
  GtkTreeSelection* selection = (GtkTreeSelection*) user_data;
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const dp = find_display_plugin(is_selection_name);
  if (dp) {
    NOTIFICATION_INFO* const ni = g_new0(NOTIFICATION_INFO, 1);
    ni->title = g_strdup("Preview Display");
    ni->text = g_strdup_printf(
        "This is a preview of the '%s' display.", dp->name());
    ni->icon = g_build_filename(DATADIR, "data", "mattn.png", NULL);
    ni->local = TRUE;
    g_idle_add((GSourceFunc) dp->show, ni);
  }
  g_free(name);
}

static gboolean
password_focus_out(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  g_free(password);
  password = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
  set_config_string("password", password);
  return FALSE;
}

static void
require_password_for_local_apps_changed(
    GtkToggleButton *togglebutton, gpointer user_data) {
  require_password_for_local_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_local_apps",
      require_password_for_local_apps);
}

static void
require_password_for_lan_apps_changed(
    GtkToggleButton *togglebutton, gpointer user_data) {
  require_password_for_lan_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_lan_apps",
      require_password_for_lan_apps);
}

static void
subscriber_enable_toggled(
    GtkCellRendererToggle *cell, gchar* path_str, gpointer user_data) {

  GtkTreeModel* const model = (GtkTreeModel *) user_data;
  GtkTreeIter iter;
  GtkTreePath* const path = gtk_tree_path_new_from_string (path_str);
  gboolean enable;
  gchar* name;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter, 0, &enable, 1, &name, -1);
  enable = !enable;
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, enable, -1);

  exec_splite3("delete from subscriber where name = '%q'", name);

  exec_splite3(
    "insert into subscriber(name, enable) values('%q', %d)",
    name, enable ? 1 : 0);

  bool
  is_model_name(const SUBSCRIBE_PLUGIN* sp) {
    return !g_strcasecmp(sp->name(), name);
  }
  SUBSCRIBE_PLUGIN* const sp = find_subscribe_plugin(is_model_name);
  if (sp) {
    enable ? sp->start() : sp->stop();
  }

  g_free(name);
  gtk_tree_path_free(path);
}

static void
notification_tree_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = g_object_get_data(G_OBJECT(user_data), "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = g_object_get_data(G_OBJECT(user_data), "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) return;

  GtkWidget* const cbx1
    = (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "enable");
  gtk_widget_set_sensitive(cbx1, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbx1), -1);
  GtkWidget* const cbx2
    = (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "display");
  gtk_widget_set_sensitive(cbx2, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbx2), -1);

  char* const sql = sqlite3_mprintf(
      "select enable, display from notification"
      " where app_name = '%q' and name = '%q'", app_name, name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(cbx1),
        sqlite3_column_int(stmt, 0) != 0 ? 0 : 1);
    char* const display = (char*) sqlite3_column_text(stmt, 1);
    const size_t len = g_list_length(display_plugins);
    for (size_t i = 0; i < len; i++) {
      DISPLAY_PLUGIN* const dp
        = (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      if (!g_strcasecmp(dp->name(), display)) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(cbx2), i);
        break;
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_free(sql);

  g_free(app_name);
  g_free(name);
}

static void
notification_enable_changed(GtkComboBox *combobox, gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = g_object_get_data(G_OBJECT(user_data), "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = g_object_get_data(G_OBJECT(user_data), "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) return;

  const gint enable = gtk_combo_box_get_active(combobox) == 0 ? 1 : 0;

  char* const sql = sqlite3_mprintf(
        "update notification set enable = %d"
        " where app_name = '%q' and name = '%q'",
        enable, app_name, name);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);

  g_free(app_name);
  g_free(name);
}

static void
notification_display_changed(GtkComboBox *combobox, gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = g_object_get_data(G_OBJECT(user_data), "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = g_object_get_data(G_OBJECT(user_data), "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) return;

  gchar* const display = gtk_combo_box_get_active_text(combobox);

  exec_splite3(
    "update notification set display = '%q'"
    " where app_name = '%q' and name = '%q'",
    display, app_name, name);

  g_free(display);
  g_free(app_name);
  g_free(name);
}

static void
append_new_menu_item_from_stock(
    GtkMenuShell* const menu, const gchar* const stock, GCallback callback) {
  GtkWidget* const menu_item = gtk_image_menu_item_new_from_stock(stock, NULL);
  g_signal_connect(G_OBJECT(menu_item), "activate", callback, NULL);
  gtk_menu_shell_append(menu, menu_item);
}

static void
application_delete(GtkWidget* widget, gpointer user_data) {
  GtkTreeIter iter1;
  GtkTreeIter iter2;
  GtkWidget* tree1 = g_object_get_data(G_OBJECT(user_data), "tree1");
  GtkWidget* tree2 = g_object_get_data(G_OBJECT(user_data), "tree2");
  GtkTreeSelection* selection1
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree1));
  GtkTreeSelection* selection2
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree2));
  GtkTreeModel* model1;
  GtkTreeModel* model2;

  gboolean selected1 =
      gtk_tree_selection_get_selected(selection1, &model1, &iter1);
  gboolean selected2 =
      gtk_tree_selection_get_selected(selection2, &model2, &iter2);

  if (!selected1 && !selected2) return;

  gchar* app_name;
  gtk_tree_model_get(model1, &iter1, 0, &app_name, -1);
  if (selected2) {
    gchar* name;
    gtk_tree_model_get(model2, &iter2, 0, &name, -1);
    exec_splite3(
      "delete from notification where app_name = '%q' and name = '%q'",
      app_name, name);
    g_free(name);

    gtk_list_store_remove(GTK_LIST_STORE(model2), &iter2);
  } else {
    exec_splite3("delete from notification where app_name = '%q'", app_name);
    gtk_list_store_remove(GTK_LIST_STORE(model1), &iter1);
    gtk_list_store_clear(GTK_LIST_STORE(model2));
  }
  g_free(app_name);

  GtkWidget* cbx1
    = (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "enable");
  gtk_widget_set_sensitive(cbx1, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbx1), -1);
  GtkWidget* cbx2
    = (GtkWidget*) g_object_get_data(G_OBJECT(user_data), "display");
  gtk_widget_set_sensitive(cbx2, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbx2), -1);
}

static gboolean
tree_view_button_pressed(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
  if (event->type != GDK_BUTTON_PRESS || event->button != 3) return FALSE;

  GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return FALSE;

  GtkTreePath* path;
  gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
          (gint) event->x, (gint) event->y, &path, NULL, NULL, NULL);
  gchar* name;
  gtk_tree_model_get(model, &iter, 0, &name, -1);

  GtkWidget* contextmenu = GTK_WIDGET(user_data);
  gtk_widget_show_all(contextmenu);
  gtk_menu_popup(GTK_MENU(contextmenu), NULL, NULL, NULL, NULL,
      event ? event->button : 0, gdk_event_get_time((GdkEvent*) event));
  return TRUE;
}

typedef gboolean button_pressed_callback_t(GtkWidget*, GdkEventButton*, gpointer);
typedef void changed_callback_t(GtkTreeSelection*, gpointer);

static GtkListStore*
vertical_list_new(const char* const model_name, const char* const tree_name,
    button_pressed_callback_t* const button_pressed_callback,
    GtkWidget* const contextmenu, changed_callback_t* const changed_callback,
    const GtkWidget* const hbox, const char* const column_attribute_name)
{
  GtkListStore* const model =
    (GtkListStore *) gtk_list_store_new(1, G_TYPE_STRING);
  g_object_set_data(G_OBJECT(setting_dialog), model_name, model);

  GtkWidget* const tree_view =
    gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
  g_object_set_data(G_OBJECT(setting_dialog), tree_name, tree_view);
  g_signal_connect(G_OBJECT(tree_view), "button-press-event",
      G_CALLBACK(button_pressed_callback), contextmenu);

  GtkTreeSelection* const select =
    gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
  gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
  g_signal_connect(G_OBJECT(select), "changed",
      G_CALLBACK(changed_callback), setting_dialog);

  gtk_box_pack_start(GTK_BOX(hbox), tree_view, FALSE, FALSE, 0);

  GtkTreeViewColumn* const column =
    gtk_tree_view_column_new_with_attributes(
        column_attribute_name, gtk_cell_renderer_text_new(), "text", 0, NULL);
  gtk_tree_view_column_set_min_width(GTK_TREE_VIEW_COLUMN(column), 80);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

  return model;
}

static void
settings_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  if (setting_dialog) {
      gtk_window_present(GTK_WINDOW(setting_dialog));
      return;
  }
  setting_dialog = gtk_dialog_new_with_buttons(
      "Settings", NULL, GTK_DIALOG_MODAL,
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
  gchar* path = g_build_filename(DATADIR, "data", "icon.png", NULL);
  gtk_window_set_icon_from_file(GTK_WINDOW(setting_dialog), path, NULL);
  g_free(path);
  gtk_window_set_position(GTK_WINDOW(setting_dialog), GTK_WIN_POS_CENTER);

  GtkWidget* notebook;
  GtkWidget* contextmenu;

  notebook = gtk_notebook_new();
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(setting_dialog)->vbox), notebook);

  {
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
        hbox, gtk_label_new("Display"));

    GtkListStore* model =
      (GtkListStore *) gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
    GtkTreeSelection* select = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
        G_CALLBACK(display_tree_selection_changed), setting_dialog);
    gtk_box_pack_start(GTK_BOX(hbox), tree_view, FALSE, FALSE, 0);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
        "Display", gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_column_set_min_width(GTK_TREE_VIEW_COLUMN(column), 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    GtkWidget* vbox = gtk_vbox_new(FALSE, 20);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, FALSE, 0);
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    g_object_set_data(G_OBJECT(setting_dialog), "description", label);
    GtkWidget* align = gtk_alignment_new(0, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(align), label);
    gtk_box_pack_start(GTK_BOX(vbox), align, FALSE, FALSE, 0);
    GtkWidget* image = gtk_image_new();
    g_object_set_data(G_OBJECT(setting_dialog), "thumbnail", image);
    gtk_box_pack_start(GTK_BOX(vbox), image, TRUE, TRUE, 0);

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, FALSE, 0);

    GtkWidget* button = gtk_button_new_with_label("Set as Default");
    g_signal_connect(G_OBJECT(button), "clicked",
        G_CALLBACK(set_as_default_clicked), select);
    gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    button = gtk_button_new_with_label("Preview");
    g_signal_connect(G_OBJECT(button), "clicked",
        G_CALLBACK(preview_clicked), select);
    gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);

    void
    append_display_plugins(DISPLAY_PLUGIN* dp) {
      GtkTreeIter iter = list_store_set_after_append(
          GTK_LIST_STORE(model), 0, dp->name(), -1);
      if (dp == current_display)
        gtk_tree_selection_select_iter(select, &iter);
    }
    foreach_display_plugin(append_display_plugins);
  }

  {
    contextmenu = gtk_menu_new();
    GtkWidget* const menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_DELETE, NULL);
    g_signal_connect(G_OBJECT(menu_item), "activate",
        G_CALLBACK(application_delete), setting_dialog);
    gtk_menu_shell_append(GTK_MENU_SHELL(contextmenu), menu_item);

    GtkWidget* const hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
        hbox, gtk_label_new("Application"));

    GtkListStore* const model1 =
      vertical_list_new("model1", "tree1",
        tree_view_button_pressed, contextmenu,
        application_tree_selection_changed, hbox, "Application");

    vertical_list_new("model2", "tree2",
      tree_view_button_pressed, contextmenu,
      notification_tree_selection_changed, hbox, "Notification");

    GtkWidget* const frame = gtk_frame_new("Setting");
    gtk_box_pack_start(GTK_BOX(hbox), frame, TRUE, TRUE, 0);

    GtkWidget* const vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    {
      GtkWidget* const hbox = gtk_hbox_new(FALSE, 5);
      gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

      GtkWidget* const label = gtk_label_new("Enable:");
      gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
      GtkWidget* const combobox = gtk_combo_box_new_text();
      g_object_set_data(G_OBJECT(setting_dialog), "enable", combobox);
      gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), "Enable");
      gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), "Disable");
      gtk_widget_set_sensitive(combobox, FALSE);
      g_signal_connect(G_OBJECT(combobox), "changed",
          G_CALLBACK(notification_enable_changed), setting_dialog);
      gtk_box_pack_start(GTK_BOX(hbox), combobox, FALSE, FALSE, 0);
    }

    {
      GtkWidget* const hbox = gtk_hbox_new(FALSE, 5);
      gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

      GtkWidget* const label = gtk_label_new("Display:");
      gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
      GtkWidget* const combobox = gtk_combo_box_new_text();
      g_object_set_data(G_OBJECT(setting_dialog), "display", combobox);
      gtk_widget_set_sensitive(combobox, FALSE);
      g_signal_connect(G_OBJECT(combobox), "changed",
          G_CALLBACK(notification_display_changed), setting_dialog);
      gtk_box_pack_start(GTK_BOX(hbox), combobox, FALSE, FALSE, 0);

      {
        char* const sql = "select distinct app_name from notification order by app_name";
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
          list_store_set_after_append(
              GTK_LIST_STORE(model1), 0, sqlite3_column_text(stmt, 0), -1);
        }
        sqlite3_finalize(stmt);
      }

      void
      append_display_plugins(DISPLAY_PLUGIN* dp) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), dp->name());
      }
      foreach_display_plugin(append_display_plugins);
    }
  }

  {
    GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
        gtk_label_new("Security"));

    GtkWidget* checkbutton;
    checkbutton = gtk_check_button_new_with_label(
        "Require password for local apps");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton),
        require_password_for_local_apps);
    g_signal_connect(G_OBJECT(checkbutton), "toggled",
        G_CALLBACK(require_password_for_local_apps_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), checkbutton, FALSE, FALSE, 0);
    checkbutton = gtk_check_button_new_with_label(
        "Require password for LAN apps");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton),
        require_password_for_lan_apps);
    g_signal_connect(G_OBJECT(checkbutton), "toggled",
        G_CALLBACK(require_password_for_lan_apps_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), checkbutton, FALSE, FALSE, 0);
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    GtkWidget* label = gtk_label_new("Password:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), password);
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    g_signal_connect(G_OBJECT(entry), "focus-out-event",
        G_CALLBACK(password_focus_out), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), entry, FALSE, FALSE, 0);
  }
  
  {
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
        hbox, gtk_label_new("Subscribe"));

    GtkListStore* model = (GtkListStore *) gtk_list_store_new(
        3, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
    GtkTreeSelection* select = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    gtk_box_pack_start(GTK_BOX(hbox), tree_view, TRUE, TRUE, 0);

    GtkCellRenderer* renderer = gtk_cell_renderer_toggle_new();
    g_signal_connect(renderer, "toggled",
        G_CALLBACK(subscriber_enable_toggled), model);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Enable", renderer, "active", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Name", gtk_cell_renderer_text_new(), "text", 1, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Description", gtk_cell_renderer_text_new(), "markup", 2, NULL));

    void
    append_subscribe_plugins(SUBSCRIBE_PLUGIN* sp) {
      list_store_set_after_append(GTK_LIST_STORE(model),
          0, get_subscriber_enabled(sp->name()),
          1, sp->name(),
          2, sp->description(),
          -1);
    }
    foreach_subscribe_plugin(append_subscribe_plugins);
  }

  gtk_widget_set_size_request(setting_dialog, 500, 500);
  gtk_widget_show_all(setting_dialog);
  gtk_dialog_run(GTK_DIALOG(setting_dialog));
  gtk_widget_destroy(setting_dialog);
  gtk_widget_destroy(contextmenu);
  setting_dialog = NULL;
}

static void
about_click(GtkWidget* widget, gpointer user_data) {
  if (about_dialog) {
      gtk_window_present(GTK_WINDOW(about_dialog));
      return;
  }

  const gchar* authors[2] = {"Yasuhiro Matsumoto <mattn.jp@gmail.com>", NULL};
  gchar* contents = NULL;
  gchar* utf8 = NULL;
  GdkPixbuf* logo = NULL;

  about_dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(about_dialog), "Growl For Linux");
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about_dialog),
          "https://github.com/mattn/growl-for-linux/");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about_dialog),
          "A notification system for linux");
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about_dialog), authors);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about_dialog), PACKAGE_VERSION);
  if (g_file_get_contents("COPYING", &contents, NULL, NULL)) {
    utf8 = g_locale_to_utf8(contents, -1, NULL, NULL, NULL);
    g_free(contents);
    contents = NULL;
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(about_dialog), utf8);
    g_free(utf8);
    utf8 = NULL;
  }
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about_dialog),
      "http://mattn.kaoriya.net/");
  gchar* path = g_build_filename(DATADIR, "data", NULL);
  gchar* fullpath = g_build_filename(path, "growl4linux.jpg", NULL);
  g_free(path);
  logo = gdk_pixbuf_new_from_file(fullpath, NULL);
  g_free(fullpath);
  gtk_about_dialog_set_logo (GTK_ABOUT_DIALOG(about_dialog), logo);
  g_object_unref(G_OBJECT(logo));
  gtk_window_set_position(GTK_WINDOW(about_dialog), GTK_WIN_POS_CENTER);
  gtk_dialog_run(GTK_DIALOG(about_dialog));
  gtk_widget_destroy(about_dialog);
  about_dialog = NULL;
}

static void
exit_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  gtk_main_quit();
}

static char*
crlf_to_term_or_null(char* str) {
  str = strstr(str, "\r\n");
  if (str) {
    memcpy(str, "\0", 2);
    str += 2;
  }
  return str;
}

static char*
crlf_to_term_and_skip(char* const str) {
  char* tmp = crlf_to_term_or_null(str);
  if (!tmp) {
    tmp = str + strlen(str);
  }
  return tmp;
}

static char*
cr_to_lf(char* const str) {
  for (char* itr = str; (itr = strchr(itr, '\r')); *itr++ = '\n');
  return str;
}

static void
str_swap(char ** restrict _left, char ** restrict _right)
{
    char *tmp = *_left;
    *_left    = *_right;
    *_right   = tmp;
}

static gpointer
gntp_recv_proc(gpointer user_data) {
  int sock = (int) user_data;
  bool is_local_app = false;

  struct sockaddr_in client;
  socklen_t client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if (!getsockname(sock, (struct sockaddr *) &client, &client_len)) {
    const char* addr = inet_ntoa(((struct sockaddr_in *)(void*)&client)->sin_addr);
    if (addr && !strcmp(addr, "127.0.0.1")) {
      is_local_app = true;
    }
  }

  char* ptr = "";
  const size_t r = read_all(sock, &ptr);
  char* const top = ptr;
  if (!strncmp(ptr, "GNTP/1.0 ", 9)) {
    ptr += 9;

    const char* const command = ptr;
    if (!strncmp(command, "REGISTER ", 9)) ptr += 8;
    else if (!strncmp(command, "NOTIFY ", 7)) ptr += 6;
    else goto leave;

    *ptr++ = 0;

    char* data = NULL;
    if (!strncmp(ptr, "NONE", 4) && strchr("\r\n ", *(ptr+5))) {
      if (is_local_app && get_config_bool(
            "require_password_for_local_apps", FALSE)) goto leave;
      if (!is_local_app && get_config_bool(
            "require_password_for_lan_apps", FALSE)) goto leave;
      if (!(ptr = crlf_to_term_or_null(ptr))) goto leave;
      const size_t datalen = r - (ptr-top) - 4;
      data = (char*) malloc(datalen+1);
      if (!data) goto leave;
      memcpy(data, ptr, datalen);
      data[datalen] = '\0';
    } else {
      if (strncmp(ptr, "AES:", 4) &&
          strncmp(ptr, "DES:", 4) &&
          strncmp(ptr, "3DES:", 5)) goto leave;

      const char* const crypt_algorythm = ptr;
      if (!(ptr = strchr(ptr, ':'))) goto leave;
      *ptr++ = 0;

      char* const iv = ptr;
      if (!(ptr = strchr(ptr, ' '))) goto leave;
      *ptr++ = 0;

      if (strncmp(ptr, "MD5:", 4) &&
          strncmp(ptr, "SHA1:", 5) &&
          strncmp(ptr, "SHA256:", 7)) goto leave;

      const char* const hash_algorythm = ptr;
      if (!(ptr = strchr(ptr, ':'))) goto leave;
      *ptr++ = 0;

      char* const key = ptr;
      if (!(ptr = strchr(ptr, '.'))) goto leave;
      *ptr++ = 0;

      char* const salt = ptr;

      if (!(ptr = crlf_to_term_or_null(ptr))) goto leave;

      const size_t saltlen = strlen(salt) / 2;
      for (size_t n = 0; n < saltlen; n++)
        salt[n] = unhex(salt[n * 2]) * 16 + unhex(salt[n * 2 + 1]);
      const size_t keylen = strlen(key) / 2;
      for (size_t n = 0; n < keylen; n++)
        key[n] = unhex(key[n * 2]) * 16 + unhex(key[n * 2 + 1]);
      const size_t ivlen = strlen(iv) / 2;
      for (size_t n = 0; n < ivlen; n++)
        iv[n] = unhex(iv[n * 2]) * 16 + unhex(iv[n * 2 + 1]);

      unsigned char digest[32] = {0};
      if (!strcmp(hash_algorythm, "MD5")) {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        MD5_Update(&ctx, password, strlen(password));
        MD5_Update(&ctx, salt, saltlen);
        MD5_Final(digest, &ctx);
      }
      else if (!strcmp(hash_algorythm, "SHA1")) {
        SHA_CTX ctx;
        SHA1_Init(&ctx);
        SHA1_Update(&ctx, password, strlen(password));
        SHA1_Update(&ctx, salt, saltlen);
        SHA1_Final(digest, &ctx);
      }
      else if (!strcmp(hash_algorythm, "SHA256")) {
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, password, strlen(password));
        SHA256_Update(&ctx, salt, saltlen);
        SHA256_Final(digest, &ctx);
      }

      data = (char*) calloc(r, 1);
      if (!data) goto leave;
      if (!strcmp(crypt_algorythm, "AES")) {
        AES_KEY aeskey;
        AES_set_decrypt_key(digest, 24 * 8, &aeskey);
        AES_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &aeskey, (unsigned char*) iv, AES_DECRYPT);
      }
      else if (!strcmp(crypt_algorythm, "DES")) {
        des_key_schedule schedule;
        DES_set_key_unchecked((const_DES_cblock*) &digest, &schedule);
        DES_ncbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &schedule, (const_DES_cblock*) &iv, DES_DECRYPT);
      }
      else if (!strcmp(crypt_algorythm, "3DES")) {
        des_key_schedule schedule1, schedule2, schedule3;
        DES_set_key_unchecked((const_DES_cblock*) (digest+ 0), &schedule1);
        DES_set_key_unchecked((const_DES_cblock*) (digest+ 8), &schedule2);
        DES_set_key_unchecked((const_DES_cblock*) (digest+16), &schedule3);
        des_ede3_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, schedule1, schedule2, schedule3,
            (const_DES_cblock*) &iv, DES_DECRYPT);
      }
    }

    ptr = data;
    if (!strcmp(command, "REGISTER")) {
      char* application_name = NULL;
      char* application_icon = NULL;
      long notifications_count = 0;
      while (*ptr) {
        char* const line = ptr;
        ptr = crlf_to_term_and_skip(ptr);
        if (*line== '\0') break;
        cr_to_lf(line);

        const char* const colon = strchr(line, ':');
        if (colon)
        {
          char* value = g_strdup(skipsp(colon + 1));
          if (!strncmp(line, "Application-Name:", 17)) {
            str_swap(&value, &application_name);
          }
          else if (!strncmp(line, "Application-Icon:", 17)) {
            str_swap(&value, &application_icon);
          }
          else if (!strncmp(line, "Notifications-Count:", 20)) {
            notifications_count = atol(value);
          }
          g_free(value);
        }
      }
      int n;
      for (n = 0; n < notifications_count; n++) {
        char* notification_name = NULL;
        char* notification_icon = NULL;
        gboolean notification_enabled = FALSE;
        char* notification_display_name = NULL;
        while (*ptr) {
          char* const line = ptr;
          ptr = crlf_to_term_and_skip(ptr);
          if (*line== '\0') break;
          cr_to_lf(line);

          const char* const colon = strchr(line, ':');
          if (colon)
          {
            char* value = g_strdup(skipsp(colon + 1));
            if (!strncmp(line, "Notification-Name:", 18)) {
              str_swap(&value, &notification_name);
            }
            else if (!strncmp(line, "Notification-Icon:", 18)) {
              str_swap(&value, &notification_icon);
            }
            else if (!strncmp(line, "Notification-Enabled:", 21)) {
              notification_enabled = strcasecmp(value, "true") == 0;
            }
            else if (!strncmp(line, "Notification-Display-Name:", 26)) {
              str_swap(&value, &notification_display_name);
            }
            g_free(value);
          }
        }

        exec_splite3(
          "delete from notification where app_name = '%q' and name = '%q'",
          application_name, notification_name);

        exec_splite3(
          "insert into notification("
          "app_name, app_icon, name, icon, enable, display, sticky)"
          " values('%q', '%q', '%q', '%q', %d, '%q', %d)",
          application_name,
          application_icon ? application_icon : "",
          notification_name,
          notification_icon ? notification_icon : "",
          notification_enabled,
          notification_display_name ?
            notification_display_name : "Default",
          FALSE);

        g_free(notification_name);
        g_free(notification_icon);
        g_free(notification_display_name);
      }
      ptr = n == notifications_count
        ? GNTP_OK_STRING_LITERAL("1.0", "REGISTER")
        : GNTP_ERROR_STRING_LITERAL("1.0", "Invalid data", "Invalid data");
      send(sock, ptr, strlen(ptr), 0);
      g_free(application_name);
      g_free(application_icon);
    } else {
      NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
      if (!ni) {
        perror("g_new0");
        goto leave;
      }
      char* application_name = NULL;
      char* notification_name = NULL;
      char* notification_display_name = NULL;
      while (*ptr) {
        char* const line = ptr;
        ptr = crlf_to_term_and_skip(ptr);
        if (*line == '\0') break;
        cr_to_lf(line);

        const char* const colon = strchr(line, ':');
        if (colon)
        {
          char* value = g_strdup(skipsp(colon + 1));
          if (!strncmp(line, "Application-Name:", 17)) {
            str_swap(&value, &application_name);
          }
          else if (!strncmp(line, "Notification-Name:", 18)) {
            str_swap(&value, &notification_name);
          }
          else if (!strncmp(line, "Notification-Title:", 19)) {
            str_swap(&value, &ni->title);
          }
          else if (!strncmp(line, "Notification-Text:", 18)) {
            str_swap(&value, &ni->text);
          }
          else if (!strncmp(line, "Notification-Icon:", 18)) {
            str_swap(&value, &ni->icon);
          }
          else if (!strncmp(line, "Notification-Callback-Target:", 29)) {
            str_swap(&value, &ni->url);
          }
          else if (!strncmp(line, "Notification-Display-Name:", 26)) {
            str_swap(&value, &notification_display_name);
          }
          g_free(value);
        }
      }

      if (ni->title && ni->text) {
        ptr = g_strdup_printf(GNTP_OK_STRING_LITERAL("1.0", "%s"), command);
        send(sock, ptr, strlen(ptr), 0);

        gboolean enable = FALSE;
        char* const sql = sqlite3_mprintf(
            "select enable, display from notification"
            " where app_name = '%q' and name = '%q'",
            application_name, notification_name);
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          enable = (gboolean) sqlite3_column_int(stmt, 0);
          if (!notification_display_name)
              notification_display_name =
                g_strdup((char*) sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);
        sqlite3_free(sql);

        if (enable) {
          DISPLAY_PLUGIN* cp = current_display;
          if (notification_display_name) {
            bool
            is_notification_display(const DISPLAY_PLUGIN* dp) {
              return !g_strcasecmp(dp->name(), notification_display_name);
            }
            cp = find_display_plugin_or(is_notification_display, cp);
          }
          g_idle_add((GSourceFunc) cp->show, ni); // call once
        }
      } else {
        ptr = GNTP_ERROR_STRING_LITERAL("1.0", "Invalid data", "Invalid data");
        send(sock, ptr, strlen(ptr), 0);

        g_free(ni->title);
        g_free(ni->text);
        g_free(ni->icon);
        g_free(ni->url);
        g_free(ni);
      }
      g_free(notification_name);
      g_free(notification_display_name);
      g_free(application_name);
    }
    free(data);
  } else {
    ptr = GNTP_ERROR_STRING_LITERAL("1.0", "Invalid command", "Invalid command");
    send(sock, ptr, strlen(ptr), 0);
  }
  free(top);
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  return NULL;

leave:
  ptr = GNTP_ERROR_STRING_LITERAL("1.0", "Invalid request", "Invalid request");
  send(sock, ptr, strlen(ptr), 0);
  free(top);
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  return NULL;
}

#ifdef _WIN32
static BOOL WINAPI
ctrl_handler(DWORD type) {
  if (about_dialog) gtk_dialog_response(GTK_DIALOG(about_dialog), GTK_RESPONSE_CLOSE);
  if (setting_dialog) gtk_dialog_response(GTK_DIALOG(setting_dialog), GTK_RESPONSE_CLOSE);

  SetConsoleCtrlHandler(ctrl_handler, TRUE);
  gtk_main_quit();
  return TRUE;
}
#else
static void
signal_handler(int num) {
  if (about_dialog) gtk_dialog_response(GTK_DIALOG(about_dialog), GTK_RESPONSE_CLOSE);
  if (setting_dialog) gtk_dialog_response(GTK_DIALOG(setting_dialog), GTK_RESPONSE_CLOSE);

  signal(num, signal_handler);
  gtk_main_quit();
}
#endif

/*
static GdkPixbuf*
disabled_pixbuf(GdkPixbuf *pixbuf) {
  GdkPixbuf* gray = gdk_pixbuf_copy(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(gray);
  int pixstride = gdk_pixbuf_get_has_alpha(gray) ? 4 : 3;
  guchar* pixels = gdk_pixbuf_get_pixels(gray);
  int n_rows = gdk_pixbuf_get_height(gray);
  int width = gdk_pixbuf_get_width(gray);
  int row = 0;

  while (row < n_rows) {
    guchar *p = pixels + row * rowstride;
    guchar *end = p + (pixstride * width);
    while (p != end) {
#define INTENSITY(r, g, b) ((r) * 0.30 + (g) * 0.59 + (b) * 0.11)
      double v = INTENSITY(p[0], p[1], p[2]);
#undef INTENSITY
      p[0] = (guchar) v;
      p[1] = (guchar) v;
      p[2] = (guchar) v;
      p += pixstride;
    }
    row++;
  }
  return gray;
}
*/

static void
create_menu() {
  {
    // TODO: absolute path
    gchar* const path = g_build_filename(DATADIR, "data", NULL);
    gchar* const fullpath = g_build_filename(path, "icon.png", NULL);
    g_free(path);
    status_icon = gtk_status_icon_new_from_file(fullpath);
    g_free(fullpath);
    gtk_status_icon_set_tooltip(status_icon, "Growl");
    gtk_status_icon_set_visible(status_icon, TRUE);
  }

  popup_menu = gtk_menu_new();
  g_signal_connect(G_OBJECT(status_icon), "popup-menu",
      G_CALLBACK(status_icon_popup), popup_menu);

  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_PREFERENCES, G_CALLBACK(settings_clicked));
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_ABOUT, G_CALLBACK(about_click));
  gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu),
    gtk_separator_menu_item_new());
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_QUIT, G_CALLBACK(exit_clicked));
  gtk_widget_show_all(popup_menu);
}

static void
destroy_menu() {
  if (popup_menu) {
      gtk_widget_destroy(popup_menu);
  }
  if (status_icon) {
      gtk_status_icon_set_visible(GTK_STATUS_ICON(status_icon), FALSE);
      g_object_unref(G_OBJECT(status_icon));
  }
}

static gboolean
load_config() {
  const gchar* const confdir = (const gchar*) g_get_user_config_dir();
  gchar* const appdir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", NULL);
  if (g_mkdir_with_parents(appdir, 0700) < 0) {
    perror("mkdir");
    g_critical("Can't create directory: %s", appdir);
    g_free(appdir);
    return FALSE;
  }
  gchar* const confdb = g_build_filename(appdir, "config.db", NULL);
  g_free(appdir);
  const gboolean exist = g_file_test(confdb, G_FILE_TEST_EXISTS);
  if (sqlite3_open(confdb, &db) != SQLITE_OK) {
    g_critical("Can't open database: %s", confdb);
    g_free(confdb);
    return FALSE;
  }
  g_free(confdb);

  if (!exist) {
    if (sqlite3_exec(db, "create table config"
          "(key text not null primary key, value text not null)",
          NULL, NULL, NULL) != SQLITE_OK) {
      g_critical("Can't create configuration table");
      return FALSE;
    }
  }

  gchar* const version = get_config_string("version", "");
  if (strcmp(version, PACKAGE_VERSION)) {
    const char* sqls[] = {
      "drop table _notification",
      "drop table _subscriber",
      "alter table notification rename to _notification",
      "alter table subscriber rename to _subscriber",
      "create table notification("
          "app_name text not null,"
          "app_icon text not null,"
          "name text not null,"
          "icon text not null,"
          "enable int not null,"
          "display text not null,"
          "sticky int not null,"
          "primary key(app_name, name))",
      "create table subscriber("
          "name text not null primary key,"
          "enable int not null)",
      "insert into notification from select * from _notification",
      "insert into subscriber from select * from subscriber",
      "drop table _notification",
      "drop table _subscriber",
      NULL
    };
    for (const char* const* sql = sqls; *sql; ++sql) {
      sqlite3_exec(db, *sql, NULL, NULL, NULL);
    }
    set_config_string("version", PACKAGE_VERSION);
  }
  g_free(version);

  password = get_config_string("password", "");
  require_password_for_local_apps =
    get_config_bool("require_password_for_local_apps", FALSE);
  require_password_for_lan_apps =
    get_config_bool("require_password_for_lan_apps", FALSE);

  return TRUE;
}

static void
unload_config() {
  g_free(password);
  if (db) sqlite3_close(db);
}

static gboolean
load_display_plugins() {
  gchar* const path = g_build_filename(LIBDIR, "display", NULL);
  GDir* const dir = g_dir_open(path, 0, NULL);
  if (!dir) {
    perror("open");
    g_critical("Display plugin directory isn't found: %s", path);
    return FALSE;
  }

  gchar* const default_display = get_config_string("default_display", "Default");

  current_display = NULL;
  const gchar *filename;
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* const fullpath = g_build_filename(path, filename, NULL);
    GModule* const handle = g_module_open(fullpath, G_MODULE_BIND_LAZY);
    g_free(fullpath);
    if (!handle) {
      continue;
    }
    DISPLAY_PLUGIN* const dp = g_new0(DISPLAY_PLUGIN, 1);
    dp->handle = handle;
    g_module_symbol(handle, "display_show", (void**) &dp->show);
    g_module_symbol(handle, "display_init", (void**) &dp->init);
    g_module_symbol(handle, "display_term", (void**) &dp->term);
    g_module_symbol(handle, "display_name", (void**) &dp->name);
    g_module_symbol(handle, "display_description", (void**) &dp->description);
    g_module_symbol(handle, "display_thumbnail", (void**) &dp->thumbnail);
    if (dp->init && !dp->init()) {
      g_module_close(dp->handle);
      g_free(dp);
      continue;
    }
    display_plugins = g_list_append(display_plugins, dp);
    if (dp && dp->name &&
        !g_strcasecmp(dp->name(), default_display)) current_display = dp;
  }

  g_dir_close(dir);
  g_free(path);
  g_free(default_display);

  if (g_list_length(display_plugins) == 0) {
    g_critical("No display plugins found.");
    return FALSE;
  }

  if (!current_display) current_display = g_list_nth_data(display_plugins, 0);

  return TRUE;
}

static void
unload_display_plugins() {
  void
  close_plugin(DISPLAY_PLUGIN* dp) {
    if (dp->term) dp->term();
    g_module_close(dp->handle);
    g_free(dp);
  }
  foreach_display_plugin(close_plugin);
  g_list_free(display_plugins);
  display_plugins = NULL;
}

static void
subscribe_show(NOTIFICATION_INFO* const ni) {
  g_idle_add((GSourceFunc) current_display->show, ni);
}

static gboolean
load_subscribe_plugins() {
  gchar* const path = g_build_filename(LIBDIR, "subscribe", NULL);
  GDir* const dir = g_dir_open(path, 0, NULL);
  if (!dir) {
    g_warning("Subscribe plugin directory isn't found: %s", path);
    return TRUE;
  }

  sc.show = subscribe_show;
  const gchar *filename;
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* const fullpath = g_build_filename(path, filename, NULL);
    GModule* const handle = g_module_open(fullpath, G_MODULE_BIND_LAZY);
    g_free(fullpath);
    if (!handle) {
      continue;
    }
    SUBSCRIBE_PLUGIN* const sp = g_new0(SUBSCRIBE_PLUGIN, 1);
    sp->handle = handle;
    g_module_symbol(handle, "subscribe_start", (void**) &sp->start);
    g_module_symbol(handle, "subscribe_stop", (void**) &sp->stop);
    g_module_symbol(handle, "subscribe_init", (void**) &sp->init);
    g_module_symbol(handle, "subscribe_term", (void**) &sp->term);
    g_module_symbol(handle, "subscribe_name", (void**) &sp->name);
    g_module_symbol(handle, "subscribe_description", (void**) &sp->description);
    g_module_symbol(handle, "subscribe_thumbnail", (void**) &sp->thumbnail);
    if (sp->init && !sp->init(&sc)) {
      g_module_close(sp->handle);
      g_free(sp);
      continue;
    }
    if (get_subscriber_enabled(sp->name())) sp->start();
    subscribe_plugins = g_list_append(subscribe_plugins, sp);
  }

  g_dir_close(dir);
  g_free(path);

  return TRUE;
}

static void
unload_subscribe_plugins() {
  void
  close_plugin(SUBSCRIBE_PLUGIN* sp) {
    if (sp->term) sp->term();
    g_module_close(sp->handle);
    g_free(sp);
  }
  foreach_subscribe_plugin(close_plugin);
  g_list_free(subscribe_plugins);
  subscribe_plugins = NULL;
}

static gboolean
gntp_accepted(GIOChannel* const source, GIOCondition condition, gpointer user_data) {
  int fd = g_io_channel_unix_get_fd(source);
  int sock;
  struct sockaddr_in client;
  socklen_t client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if ((sock = accept(fd, (struct sockaddr *) &client, &client_len)) < 0) {
    perror("accept");
    return TRUE;
  }

#ifdef G_THREADS_ENABLED
  g_thread_create(gntp_recv_proc, (gpointer) sock, FALSE, NULL);
#else
  gntp_recv_proc((gpointer) sock);
#endif
  return TRUE;
}

typedef struct {
  unsigned char ver;
  unsigned char type;
  unsigned short app_name_length;
  unsigned char nall;
  unsigned char ndef;
} GROWL_REGIST_PACKET;

typedef struct {
  unsigned char ver;
  unsigned char type;
  unsigned short flags;
  unsigned short notification_length;
  unsigned short title_length;
  unsigned short description_length;
  unsigned short app_name_length;
} GROWL_NOTIFY_PACKET;

static gboolean
udp_recv_proc(GIOChannel* const source, GIOCondition condition, gpointer user_data) {
  int is_local_app = FALSE;
  int fd = g_io_channel_unix_get_fd(source);
  char buf[BUFSIZ] = {0};

  const ssize_t len = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
  if (len > 0) {
    if (buf[0] == 1) {
      if (buf[1] == 0 || buf[1] == 2 || buf[1] == 4) {
        //GROWL_REGIST_PACKET* packet = (GROWL_REGIST_PACKET*) &buf[0];
      } else
      if (buf[1] == 1 || buf[1] == 3 || buf[1] == 5) {
        GROWL_NOTIFY_PACKET* packet = (GROWL_NOTIFY_PACKET*) &buf[0];
#define HASH_DIGEST_CHECK(_hash_algorithm, _password, _data, _datalen) \
{ \
  unsigned char digest[GOL_PP_JOIN(_hash_algorithm, _DIGEST_LENGTH)] = {0}; \
  const size_t datalen = _datalen - sizeof(digest);\
  GOL_PP_JOIN(_hash_algorithm, _CTX) ctx;\
  GOL_PP_JOIN(_hash_algorithm, _Init)(&ctx);\
  GOL_PP_JOIN(_hash_algorithm, _Update)(&ctx, _data, datalen);\
  GOL_PP_JOIN(_hash_algorithm, _Update)(&ctx, _password, strlen(_password));\
  GOL_PP_JOIN(_hash_algorithm, _Final)(digest, &ctx);\
  if (memcmp(digest, _data + datalen, sizeof(digest))) {\
    return TRUE;\
  }\
}
        if (packet->type == 1) {
          HASH_DIGEST_CHECK(MD5, password, buf, len);
        }
        else if (packet->type == 3) {
          HASH_DIGEST_CHECK(SHA256, password, buf, len);
        } else {
          if (is_local_app && require_password_for_local_apps) goto leave;
          if (!is_local_app && require_password_for_lan_apps) goto leave;
        }
#undef HASH_DIGEST_CHECK
        NOTIFICATION_INFO* const ni = g_new0(NOTIFICATION_INFO, 1);
        ni->title = g_strndup(
            &buf[sizeof(GROWL_NOTIFY_PACKET)
              + ntohs(packet->notification_length)],
                  ntohs(packet->title_length));
        ni->text = g_strndup(
            &buf[sizeof(GROWL_NOTIFY_PACKET)
              + ntohs(packet->notification_length)
              + ntohs(packet->title_length)],
                  ntohs(packet->description_length));
        ni->local = TRUE;
        g_idle_add((GSourceFunc) current_display->show, ni); // call once
      }
    }
  }
leave:

  return TRUE;
}

static GIOChannel*
create_udp_server() {
  int fd;
  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("socket");
    return NULL;
  }

  const struct sockaddr_in server_addr =
  {
    .sin_family      = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_ANY),
    .sin_port        = htons(9887),
  };

  if (bind(fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    return NULL;
  }

  fd_set fdset;
  FD_SET(fd, &fdset);
  GIOChannel* const channel = g_io_channel_unix_new(fd);
  g_io_add_watch(channel, G_IO_IN | G_IO_ERR, udp_recv_proc, NULL);
  g_io_channel_unref(channel);

  return channel;
}

static GIOChannel*
create_gntp_server() {
  int fd;
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return NULL;
  }

  const sockopt_t sockopt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
        &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    return NULL;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
        &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    return NULL;
  }

  const struct sockaddr_in server_addr =
  {
    .sin_family      = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_ANY),
    .sin_port        = htons(23053),
  };

  if (bind(fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    return NULL;
  }

  if (listen(fd, SOMAXCONN) < 0) {
    perror("listen");
    closesocket(fd);
    return NULL;
  }

  fd_set fdset;
  FD_SET(fd, &fdset);
  GIOChannel* const channel = g_io_channel_unix_new(fd);
  g_io_add_watch(channel, G_IO_IN | G_IO_ERR, gntp_accepted, NULL);
  g_io_channel_unref(channel);

  return channel;
}



static void
destroy_gntp_server(GIOChannel* const channel) {
  if (channel) {
    closesocket(g_io_channel_unix_get_fd(channel));
    g_io_channel_unref(channel);
  }
}

static void
destroy_udp_server(GIOChannel* const channel) {
  if (channel) {
    closesocket(g_io_channel_unix_get_fd(channel));
    g_io_channel_unref(channel);
  }
}

int
main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
  GIOChannel* gntp_io = NULL;
  GIOChannel* udp_io = NULL;

  gchar* program = g_find_program_in_path(argv[0]);
  exepath = g_path_get_dirname(program);
  g_free(program);

#ifdef G_THREADS_ENABLED
  g_thread_init(NULL);
#endif

  gtk_init(&argc, &argv);

#ifdef _WIN32
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);
#endif

  if (!load_config()) goto leave;
  if ((gntp_io = create_gntp_server()) == NULL) goto leave;
  if ((udp_io = create_udp_server()) == NULL) goto leave;
  if (!load_display_plugins()) goto leave;
  if (!load_subscribe_plugins()) goto leave;
  create_menu();

  gtk_main();

leave:
  destroy_menu();
  unload_subscribe_plugins();
  unload_display_plugins();
  destroy_gntp_server(gntp_io);
  destroy_udp_server(udp_io);
  unload_config();
  g_free(exepath);

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

// vim:set et sw=2 ts=2 ai:

