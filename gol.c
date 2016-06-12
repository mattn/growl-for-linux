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
#ifdef _WIN32
# include <ws2tcpip.h>
#endif
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
#else
# include <sys/socket.h>
# include <netinet/tcp.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <unistd.h>
#endif
#include <sqlite3.h>
#ifdef _WIN32
# include <io.h>
#endif
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/des.h>

#include "gol.h"
#include "compatibility.h"

#ifdef HAVE_APP_INDICATOR
#include <libappindicator/app-indicator.h>
#endif

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
  const gchar* (*name)();
  const gchar* (*description)();
  gchar** (*thumbnail)();
  void (*set_param)(const gchar*);
  gchar* (*get_param)();
} SUBSCRIBE_PLUGIN;

typedef struct {
  void* handle;
  gboolean (*init)();
  gboolean (*show)(NOTIFICATION_INFO* ni);
  gboolean (*term)();
  const gchar* (*name)();
  const gchar* (*description)();
  gchar** (*thumbnail)();
  void (*set_param)(const gchar*);
  gchar* (*get_param)();
} DISPLAY_PLUGIN;

typedef enum
{
  GOL_STATUS_NORMAL = 0,
  GOL_STATUS_DND,
} status_t;

static gchar* password;
static gboolean require_password_for_local_apps = FALSE;
static gboolean require_password_for_lan_apps = FALSE;
static sqlite3 *db;
#ifdef HAVE_APP_INDICATOR
static AppIndicator* indicator;
#else
static GtkStatusIcon* status_icon;
#endif
static GdkPixbuf* status_icon_pixbuf, * status_icon_dnd_pixbuf;
static status_t gol_status;
static GtkWidget* popup_menu;
static GtkWidget* setting_dialog;
static GtkWidget* about_dialog;
static GList* display_plugins;
static GList* subscribe_plugins;
static DISPLAY_PLUGIN* current_display;
static gchar* exepath;
static SUBSCRIPTOR_CONTEXT sc;

#ifndef LIBDIR
# define LIBDIR exepath
#endif
#ifndef DATADIR
# define DATADIR exepath
#endif

static const char*
skipsp(const char* str) {
  for (; isspace(*str); ++str);
  return str;
}

static void*
safely_realloc(void* ptr, const size_t newsize) {
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
  const struct timeval timeout = {
    .tv_sec  = 1,
    .tv_usec = 0,
  };

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (sockopt_t*) &timeout, sizeof(timeout));

  const size_t len = 1024;
  size_t bufferlen = len;
  char* buf = (char*) malloc(bufferlen + 1);
  if (!buf) {
    perror("malloc");
    return 0;
  }

  char* end = buf;
  ptrdiff_t datalen = 0;
  int retry = 3;
  while (1) {
    ssize_t r = recv(fd, end, bufferlen - datalen, 0);
    if (r <= 0) {
      if (--retry < 0) break; 
#ifdef _WIN32
      DWORD err = GetLastError();
      if (err == WSAEWOULDBLOCK) continue;
#else
      int err = errno;
      if (err == EWOULDBLOCK || err == EAGAIN) continue; 
#endif
      break; 
    }
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
  wrapped_pred(gconstpointer dp, gconstpointer GOL_UNUSED_ARG(user_data)) {
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
  wrapped_pred(gconstpointer sp, gconstpointer GOL_UNUSED_ARG(user_data)) {
    return pred((const SUBSCRIBE_PLUGIN*) sp) ? 0 : 1;
  }
  GList* elem = g_list_find_custom(subscribe_plugins, NULL, wrapped_pred);
  return elem ? (SUBSCRIBE_PLUGIN*) elem->data : NULL;
}

void
foreach_display_plugin(void(* func)(DISPLAY_PLUGIN*)) {
  void
  wrapped_func(gpointer dp, gpointer GOL_UNUSED_ARG(user_data)) {
    func((DISPLAY_PLUGIN*) dp);
  }
  g_list_foreach(display_plugins, wrapped_func, NULL);
}

void
foreach_subscribe_plugin(void(* func)(SUBSCRIBE_PLUGIN*)) {
  void
  wrapped_func(gpointer sp, gpointer GOL_UNUSED_ARG(user_data)) {
    func((SUBSCRIBE_PLUGIN*) sp);
  }
  g_list_foreach(subscribe_plugins, wrapped_func, NULL);
}

static void
exec_sqlite3(const char tsql[], ...) {
  va_list list;
  va_start(list, tsql);

  char* const sql = sqlite3_vmprintf(tsql, list);
  gol_debug_message("request \n\t\"%s\"", sql);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
    gol_debug_warning("sqlite3 reports an error.\n\t%s", sqlite3_errmsg(db));
  sqlite3_free(sql);

  va_end(list);
}

static sqlite3_stmt *
vprepare_sqlite3(const char* const tsql, va_list list) {
  char* const sql = sqlite3_vmprintf(tsql, list);
  gol_debug_message("request \n\t\"%s\"", sql);
  sqlite3_stmt* stmt = NULL;
  if (sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK)
    gol_debug_warning("sqlite3 reports an error.\n\t%s", sqlite3_errmsg(db));
  sqlite3_free(sql);
  return stmt;
}

static sqlite3_stmt *
prepare_sqlite3(const char* const tsql, ...) {
  va_list list;
  va_start(list, tsql);

  sqlite3_stmt* const stmt = vprepare_sqlite3(tsql, list);

  va_end(list);
  return stmt;
}

static void
statement_sqlite3(void(* stmt_func)(sqlite3_stmt*), const char* const tsql, ...) {
  va_list list;
  va_start(list, tsql);

  sqlite3_stmt* const stmt = vprepare_sqlite3(tsql, list);
  stmt_func(stmt);

  sqlite3_finalize(stmt);

  va_end(list);
}

static GdkPixbuf*
pixbuf_from_datadir(const gchar* filename, GError** error) {
  if (!filename) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "filename should not be NULL");
    return NULL;
  }

  gchar* const path = g_build_filename(DATADIR, "data", filename, NULL);
  GdkPixbuf* const pixbuf = gdk_pixbuf_new_from_file(path, error);
  g_free(path);

  return pixbuf;
}

static gboolean
get_config_bool(const char* key, gboolean def) {
  gboolean ret;
  void
  get_int(sqlite3_stmt* const stmt) {
    ret = sqlite3_step(stmt) == SQLITE_ROW
      ? (gboolean) sqlite3_column_int(stmt, 0)
      : def;
  }
  statement_sqlite3(get_int,
      "select value from config where key = '%q'", key);
  return ret;
}

static gboolean
get_subscriber_enabled(const char* name) {
  gboolean ret;
  void
  get_enabled(sqlite3_stmt* const stmt) {
    ret = sqlite3_step(stmt) == SQLITE_ROW
        ? (gboolean) sqlite3_column_int(stmt, 0)
        : FALSE;
  }
  statement_sqlite3(get_enabled,
      "select enable from subscriber where name = '%q'", name);
  return ret;
}

static gchar*
get_display_parameter(const char* const name, const char* const def) {
  gchar* value;
  void
  get_string(sqlite3_stmt* const stmt) {
    value = g_strdup(
        sqlite3_step(stmt) == SQLITE_ROW
          ? (char*) sqlite3_column_text(stmt, 0)
          : def ? def : "");
  }
  statement_sqlite3(get_string,
      "select parameter from display where name = '%q'", name);
  return value;
}

static void
set_display_parameter(const char* const name, const char* const value) {
  gol_debug_message("name: %s, parameter: %s", name, value);
  exec_sqlite3("delete from display where name = '%q'", name);

  exec_sqlite3("insert into display(name, parameter) values('%q', '%q')", name, value);
}

static gint
get_config_value(const char* const key, const gint def) {
  gchar* data;
  gint value;
  void
  get_string(sqlite3_stmt* const stmt) {
    data = sqlite3_step(stmt) == SQLITE_ROW
          ? (char*) sqlite3_column_text(stmt, 0): "";
  }
  statement_sqlite3(get_string,
      "select value from config where key = '%q'", key);
  value = g_ascii_strtoll(data, NULL, 10);
  return value != 0 ? value : def;
}

static gchar*
get_config_string(const char* const key, const char* const def) {
  gchar* value;
  void
  get_string(sqlite3_stmt* const stmt) {
    value = g_strdup(
        sqlite3_step(stmt) == SQLITE_ROW
          ? (char*) sqlite3_column_text(stmt, 0)
          : def ? def : "");
  }
  statement_sqlite3(get_string,
      "select value from config where key = '%q'", key);
  return value;
}

static void
set_config_string(const char* const key, const char* const value) {
  exec_sqlite3("delete from config where key = '%q'", key);
  exec_sqlite3("insert into config(key, value) values('%q', '%q')", key, value);
}

static void
set_config_bool(const char* key, gboolean value) {
  set_config_string(key, value ? "1" : "0");
}

static bool
get_tree_model_from_selection(gchar** const restrict pname, GtkTreeSelection* const restrict selection) {
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (!GTK_IS_TREE_SELECTION(selection)) return FALSE;
  if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return FALSE;

  gtk_tree_model_get(model, &iter, 0, pname, -1);
  return TRUE;
}

static bool
get_tree_model_from_tree(gchar** const restrict pname, GtkWidget* const restrict tree) {
  GtkTreeSelection* const selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  return get_tree_model_from_selection(pname, selection);
}

static GtkTreeIter
list_store_set_before_prepand(GtkListStore* const list_store, ...) {
  va_list list;
  va_start(list, list_store);
  GtkTreeIter iter;
  gtk_list_store_prepend(list_store, &iter);
  gtk_list_store_set_valist(list_store, &iter, list);
  va_end(list);
  return iter;
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

static gpointer
get_data_as_object(gpointer user_data, const gchar* const key) {
  return g_object_get_data(G_OBJECT(user_data), key);
}

static GtkComboBox*
combo_box_set_active_as_object(gpointer user_data, const gchar* const key, const int index) {
  GtkWidget* const cbx = (GtkWidget*) get_data_as_object(user_data, key);
  gtk_widget_set_sensitive(cbx, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbx), index);
  return GTK_COMBO_BOX(cbx);
}

static void
display_tree_selection_changed(GtkTreeSelection * const selection, const gpointer user_data) {
  gchar* name;

  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_ascii_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const cp = find_display_plugin_or(is_selection_name, current_display);

  GtkWidget* const label = (GtkWidget*) get_data_as_object(user_data, "description");
  gtk_label_set_markup(GTK_LABEL(label), "");
  if (cp->description) {
    gtk_label_set_markup(GTK_LABEL(label), cp->description());
  }

  GtkWidget* const image = (GtkWidget*) get_data_as_object(user_data, "thumbnail");
  gtk_image_clear(GTK_IMAGE(image));
  if (cp->thumbnail) {
    char ** const raw_xpm = cp->thumbnail();
    if (raw_xpm) {
      GdkBitmap* bitmap;
      GdkPixmap* pixmap = gdk_pixmap_colormap_create_from_xpm_d(
          NULL, gdk_colormap_get_system(), &bitmap, NULL, raw_xpm);
      gtk_image_set_from_pixmap(GTK_IMAGE(image), pixmap, bitmap);
      gdk_pixmap_unref(pixmap);
      gdk_bitmap_unref(bitmap);
    }
  }

  /*
  GtkWidget* const entry = (GtkWidget*) get_data_as_object(user_data, "parameter");
  gtk_entry_set_text(GTK_ENTRY(entry), "");
  gtk_widget_set_sensitive(entry, FALSE);
  if (cp->get_param != NULL && cp->set_param != NULL) {
    const gchar* param = cp->get_param();
    gtk_entry_set_text(GTK_ENTRY(entry), param ? param : "");
    gtk_widget_set_sensitive(entry, TRUE);
  }
  */
  g_free(name);
}

static void
application_tree_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
  gchar* app_name;
  if (!get_tree_model_from_selection(&app_name, selection)) return;

  GtkListStore* const model2 = (GtkListStore*) get_data_as_object(user_data, "model2");
  gtk_list_store_clear(model2);

  void
  append_applications(sqlite3_stmt* const stmt) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      list_store_set_after_append(
          GTK_LIST_STORE(model2), 0, sqlite3_column_text(stmt, 0), -1);
    }
  }
  statement_sqlite3(append_applications,
      "select distinct name from application"
      " where app_name = '%q' order by name", app_name);

  g_free(app_name);

  gtk_widget_set_sensitive((GtkWidget*) get_data_as_object(user_data, "enable"), FALSE);
  gtk_widget_set_sensitive((GtkWidget*) get_data_as_object(user_data, "display"), FALSE);
}

static void
set_as_default_clicked(GtkWidget* GOL_UNUSED_ARG(widget), gpointer user_data) {
  GtkTreeSelection* const selection = (GtkTreeSelection*) user_data;
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_ascii_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const dp = find_display_plugin(is_selection_name);
  if (dp) {
    current_display = dp;
    set_config_string("default_display", name);
    if (current_display->set_param)
      current_display->set_param(get_display_parameter(name, ""));
  }
  g_free(name);
}

static gboolean
parameter_focus_out(GtkWidget* GOL_UNUSED_ARG(widget), GdkEvent* GOL_UNUSED_ARG(event), gpointer user_data) {
  GtkTreeSelection* selection = (GtkTreeSelection*) user_data;
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return FALSE;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_ascii_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const dp = find_display_plugin(is_selection_name);
  if (dp) {
    GtkWidget* const entry = (GtkWidget*) get_data_as_object(user_data, "parameter");
    const gchar* param = gtk_entry_get_text(GTK_ENTRY(entry));
    dp->set_param(param);
    set_display_parameter(name, param);
  }
  g_free(name);
  return FALSE;
}

static void
preview_clicked(GtkWidget* GOL_UNUSED_ARG(widget), gpointer user_data) {
  GtkTreeSelection* selection = (GtkTreeSelection*) user_data;
  gchar* name;
  if (!get_tree_model_from_selection(&name, selection)) return;

  bool
  is_selection_name(const DISPLAY_PLUGIN* dp) {
    return !g_ascii_strcasecmp(dp->name(), name);
  }
  DISPLAY_PLUGIN* const dp = find_display_plugin(is_selection_name);
  if (dp) {
    NOTIFICATION_INFO* const ni = g_new0(NOTIFICATION_INFO, 1);
    ni->title = g_strdup("Preview Display");
    ni->text = g_strdup_printf(
        "This is a preview of the '%s' display.", dp->name());
    ni->icon = g_build_filename(DATADIR, "data", "mattn.png", NULL);
    ni->local = TRUE;
    ni->timeout = get_config_value("default_timeout", 5000)/10;
    g_idle_add((GSourceFunc) dp->show, ni);
  }
  g_free(name);
}

static gboolean
password_focus_out(GtkWidget* widget, GdkEvent* GOL_UNUSED_ARG(event), gpointer GOL_UNUSED_ARG(user_data)) {
  g_free(password);
  password = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
  set_config_string("password", password);
  return FALSE;
}

static void
require_password_for_local_apps_changed(
    GtkToggleButton *togglebutton, gpointer GOL_UNUSED_ARG(user_data)) {
  require_password_for_local_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_local_apps",
      require_password_for_local_apps);
}

static void
require_password_for_lan_apps_changed(
    GtkToggleButton *togglebutton, gpointer GOL_UNUSED_ARG(user_data)) {
  require_password_for_lan_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_lan_apps",
      require_password_for_lan_apps);
}

static void
subscriber_enable_toggled(
    GtkCellRendererToggle* GOL_UNUSED_ARG(cell), gchar* path_str, gpointer user_data) {

  GtkTreeModel* const model = (GtkTreeModel *) user_data;
  GtkTreeIter iter;
  GtkTreePath* const path = gtk_tree_path_new_from_string (path_str);
  gboolean enable;
  gchar* name;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter, 0, &enable, 1, &name, -1);
  enable = !enable;
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, enable, -1);

  exec_sqlite3("delete from subscriber where name = '%q'", name);

  exec_sqlite3(
    "insert into subscriber(name, enable) values('%q', %d)",
    name, enable ? 1 : 0);

  bool
  is_model_name(const SUBSCRIBE_PLUGIN* sp) {
    return !g_ascii_strcasecmp(sp->name(), name);
  }
  SUBSCRIBE_PLUGIN* const sp = find_subscribe_plugin(is_model_name);
  if (sp) {
    enable ? sp->start() : sp->stop();
  }

  g_free(name);
  gtk_tree_path_free(path);
}

static void
notification_tree_selection_changed(GtkTreeSelection* GOL_UNUSED_ARG(selection), gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = get_data_as_object(user_data, "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = get_data_as_object(user_data, "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) return;

  int cbx1_index = -1;
  int cbx2_index = -1;

  void
  create_combo_boxes(sqlite3_stmt* const stmt) {
    if (sqlite3_step(stmt) != SQLITE_ROW) return;
    cbx1_index = sqlite3_column_int(stmt, 0) != 0 ? 0 : 1;
    char* const display = (char*) sqlite3_column_text(stmt, 1);
    bool
    is_display(const DISPLAY_PLUGIN* dp) {
      return !g_ascii_strcasecmp(dp->name(), display);
    }
    DISPLAY_PLUGIN* const dp = find_display_plugin(is_display);
    if (dp) {
      cbx2_index = g_list_index(display_plugins, dp);
    }
  }
  statement_sqlite3(create_combo_boxes,
      "select enable, display from application"
      " where app_name = '%q' and name = '%q'", app_name, name);

  combo_box_set_active_as_object(user_data, "enable", cbx1_index);
  combo_box_set_active_as_object(user_data, "display", cbx2_index);

  g_free(app_name);
  g_free(name);
}

static void
notification_enable_changed(GtkComboBox *combobox, gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = get_data_as_object(user_data, "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = get_data_as_object(user_data, "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) {
    free(app_name);
    return;
  }

  const gint enable = gtk_combo_box_get_active(combobox) == 0 ? 1 : 0;

  exec_sqlite3(
      "update application set enable = %d"
      " where app_name = '%q' and name = '%q'",
      enable, app_name, name);

  g_free(app_name);
  g_free(name);
}

static void
notification_display_changed(GtkComboBox *combobox, gpointer user_data) {
  gchar* app_name;
  GtkWidget* const tree1 = get_data_as_object(user_data, "tree1");
  if (!get_tree_model_from_tree(&app_name, tree1)) return;

  gchar* name;
  GtkWidget* const tree2 = get_data_as_object(user_data, "tree2");
  if (!get_tree_model_from_tree(&name, tree2)) {
    g_free(app_name);
    return;
  }

  gchar* const display = gtk_combo_box_get_active_text(combobox);

  exec_sqlite3(
    "update application set display = '%q'"
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
application_delete(GtkWidget* GOL_UNUSED_ARG(widget), gpointer user_data) {
  GtkTreeIter iter1;
  GtkWidget* const tree1 = get_data_as_object(user_data, "tree1");
  GtkTreeSelection* const selection1
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree1));
  GtkTreeModel* model1;
  if (!gtk_tree_selection_get_selected(selection1, &model1, &iter1)) return;

  gchar* app_name;
  gtk_tree_model_get(model1, &iter1, 0, &app_name, -1);

  GtkTreeIter iter2;
  GtkWidget* const tree2 = get_data_as_object(user_data, "tree2");
  GtkTreeSelection* const selection2
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree2));
  GtkTreeModel* model2;
  if (gtk_tree_selection_get_selected(selection2, &model2, &iter2)) {
    gchar* name;
    gtk_tree_model_get(model2, &iter2, 0, &name, -1);
    exec_sqlite3(
      "delete from application where app_name = '%q' and name = '%q'",
      app_name, name);
    g_free(name);

    gtk_list_store_remove(GTK_LIST_STORE(model2), &iter2);
  } else {
    exec_sqlite3("delete from application where app_name = '%q'", app_name);
    gtk_list_store_remove(GTK_LIST_STORE(model1), &iter1);
    gtk_list_store_clear(GTK_LIST_STORE(model2));
  }
  g_free(app_name);

  combo_box_set_active_as_object(user_data, "enable", -1);
  combo_box_set_active_as_object(user_data, "display", -1);
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
    const GtkWidget* const hbox, const char* const column_attribute_name) {
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
settings_clicked(GtkWidget* GOL_UNUSED_ARG(widget), GdkEvent* GOL_UNUSED_ARG(event), gpointer GOL_UNUSED_ARG(user_data)) {
  if (setting_dialog) {
      gtk_window_present(GTK_WINDOW(setting_dialog));
      return;
  }
  setting_dialog = gtk_dialog_new_with_buttons(
      "Settings", NULL, GTK_DIALOG_MODAL,
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
  gchar* const path = g_build_filename(DATADIR, "data", "icon256.png", NULL);
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

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, FALSE, 0);

    /*
    GtkWidget* entry = gtk_entry_new();
    g_signal_connect(G_OBJECT(entry), "focus-out-event",
        G_CALLBACK(parameter_focus_out), NULL);
    g_object_set_data(G_OBJECT(setting_dialog), "parameter", entry);
    gtk_box_pack_end(GTK_BOX(hbox), entry, FALSE, FALSE, 0);
    label = gtk_label_new("Parameter:");
    gtk_box_pack_end(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    */

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
        char* const sql = "select distinct app_name from application order by app_name";
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

  {
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
        hbox, gtk_label_new("Notifications"));

    GtkListStore* model = (GtkListStore *) gtk_list_store_new(
        3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_object_set_data(G_OBJECT(setting_dialog), "notifications", model);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));

    GtkTreeSelection* select = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    GtkWidget* swin = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(swin), tree_view);
    gtk_box_pack_start(GTK_BOX(hbox), swin, TRUE, TRUE, 0);

    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Datetime", gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Title", gtk_cell_renderer_text_new(), "text", 1, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view),
        gtk_tree_view_column_new_with_attributes(
          "Text", gtk_cell_renderer_text_new(), "text", 2, NULL));

    {
      char* const sql = "select received, title, text from notification order by received desc";
      sqlite3_stmt *stmt = NULL;
      sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        list_store_set_after_append(
            GTK_LIST_STORE(model),
            0, sqlite3_column_text(stmt, 0),
            1, sqlite3_column_text(stmt, 1),
            2, sqlite3_column_text(stmt, 2),
            -1);
      }
      sqlite3_finalize(stmt);
    }
  }

  gtk_widget_set_size_request(setting_dialog, 500, 500);
  gtk_widget_show_all(setting_dialog);
  gtk_dialog_run(GTK_DIALOG(setting_dialog));
  gtk_widget_destroy(setting_dialog);
  gtk_widget_destroy(contextmenu);
  setting_dialog = NULL;
}

static void
about_click(GtkWidget* GOL_UNUSED_ARG(widget), gpointer GOL_UNUSED_ARG(user_data)) {
  if (about_dialog) {
      gtk_window_present(GTK_WINDOW(about_dialog));
      return;
  }

  const gchar* authors[] = {
    "Yasuhiro Matsumoto <mattn.jp@gmail.com>",
    "Kohei Takahashi <flast@flast.jp>",
    NULL
  };
  gchar* contents = NULL;

  about_dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(about_dialog), "Growl For Linux");
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about_dialog),
          "https://github.com/mattn/growl-for-linux/");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about_dialog),
          "A notification system for linux");
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about_dialog), authors);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about_dialog), PACKAGE_VERSION);
  if (g_file_get_contents("COPYING", &contents, NULL, NULL)) {
    gchar* utf8 = g_locale_to_utf8(contents, -1, NULL, NULL, NULL);
    g_free(contents);
    contents = NULL;
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(about_dialog), utf8);
    g_free(utf8);
  }
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about_dialog),
      "http://mattn.kaoriya.net/");
  {
    GdkPixbuf* logo = pixbuf_from_datadir("icon256.png", NULL);
    gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about_dialog), logo);
    g_object_unref(G_OBJECT(logo));
  }
  gtk_window_set_position(GTK_WINDOW(about_dialog), GTK_WIN_POS_CENTER);
  gtk_dialog_run(GTK_DIALOG(about_dialog));
  gtk_widget_destroy(about_dialog);
  about_dialog = NULL;
}

static void
exit_clicked(GtkWidget* GOL_UNUSED_ARG(widget), GdkEvent* GOL_UNUSED_ARG(event), gpointer GOL_UNUSED_ARG(user_data)) {
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
str_swap(char ** restrict _left, char ** restrict _right) {
    char *tmp = *_left;
    *_left    = *_right;
    *_right   = tmp;
}

typedef struct {
  const int   sock;
  const char* command;
  const char* application_name;
  const char* notification_name;
  const char* notification_display_name;
} CLIENT_INFO;

static bool
raise_notification(const CLIENT_INFO ci, NOTIFICATION_INFO* const ni) {
  const bool valid = ni && ni->title && ni->text;

  gchar* const cmd_result = valid
    ? g_strdup_printf(GNTP_OK_STRING_LITERAL("1.0", "%s"), ci.command)
    : g_strdup(GNTP_ERROR_STRING_LITERAL("1.0", "Invalid data", "Invalid data"));

  if (cmd_result) {
    send(ci.sock, cmd_result, strlen(cmd_result), 0);
    g_free(cmd_result);
  } else {
    g_critical("g_strdup or g_strdup_printf failed.");
    return false;
  }
  if (!valid) return false;

  if (gol_status == GOL_STATUS_DND)
    return true;

  DISPLAY_PLUGIN* cp = NULL;
  // Received name.
  if (ci.notification_display_name) {
    bool
    is_ndn(const DISPLAY_PLUGIN* dp) {
      return !g_ascii_strcasecmp(dp->name(), ci.notification_display_name);
    }
    cp = find_display_plugin(is_ndn);
  }

  // Lookup application default notification name.
  if (!cp) {
    sqlite3_stmt* const stmt = prepare_sqlite3(
      "select enable, display from application"
      " where app_name = '%q' and name = '%q'",
      ci.application_name, ci.notification_name);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0)) {
      const char* const adn = (const char*) sqlite3_column_text(stmt, 1);
      bool
      is_adn(const DISPLAY_PLUGIN* dp) {
        return !g_ascii_strcasecmp(dp->name(), adn);
      }
      cp = find_display_plugin(is_adn);
    }
    sqlite3_finalize(stmt);
  }

  // Lookup system default notification name.
  if (!cp) {
    const char* const sdn = get_config_string("default_display", "Fog");
    bool
    is_sdn(const DISPLAY_PLUGIN* dp) {
      return !g_ascii_strcasecmp(dp->name(), sdn);
    }
    cp = find_display_plugin_or(is_sdn, current_display);
  }

  ni->timeout = get_config_value("default_timeout", 5000)/10;
  g_idle_add((GSourceFunc) cp->show, ni); // call once
  return true;
}

static void
parse_identifiers(char* ptr) {
  const gchar* const confdir = (const gchar*) g_get_user_config_dir();
  gchar* const resourcedir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", "resource", NULL);
  if (!g_file_test(resourcedir, G_FILE_TEST_IS_DIR))
    g_mkdir_with_parents(resourcedir, 0700);
  while (*ptr) {
    char* identifier = NULL;
    long length = 0;
    while (*ptr) {
      char* const line = ptr;
      ptr = crlf_to_term_and_skip(ptr);
      if (*line == '\0') break;
      cr_to_lf(line);

      const char* const colon = strchr(line, ':');
      if (colon) {
        char* value = g_strdup(skipsp(colon + 1));
        if (!strncmp(line, "Identifier:", 11))
          identifier = g_strdup(value);
        if (identifier && !strncmp(line, "Length:", 7))
          length = atol(value);
        g_free(value);
      }
    }
    if (identifier) {
      gchar* const filename = g_build_filename(resourcedir, identifier, NULL);
      if (!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
        g_file_set_contents(filename, ptr, length, NULL);
      g_free(filename);
    }
  }
  g_free(resourcedir);
}

static gpointer
gntp_recv_proc(gpointer user_data) {
  int sock = (int)(intptr_t) user_data;
  bool is_local_app = FALSE;

  struct sockaddr_in client;
  socklen_t client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if (!getsockname(sock, (struct sockaddr *) &client, &client_len)) {
    const char* addr = inet_ntoa(((struct sockaddr_in *)(void*)&client)->sin_addr);
    if (addr && !strcmp(addr, "127.0.0.1")) {
      is_local_app = TRUE;
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
      if (is_local_app && require_password_for_local_apps) goto leave;
      if (!is_local_app && require_password_for_lan_apps) goto leave;
      if (!(ptr = crlf_to_term_or_null(ptr))) goto leave;
      const size_t datalen = r - (ptr-top) - 4;
      data = (char*) malloc(datalen+1);
      if (!data) goto leave;
      memcpy(data, ptr, datalen);
      data[datalen] = '\0';
    } else {
      if (strncmp(ptr, "NONE ", 5) &&
          strncmp(ptr, "AES:", 4) &&
          strncmp(ptr, "DES:", 4) &&
          strncmp(ptr, "3DES:", 5)) goto leave;

      const char* const crypt_algorythm = ptr;
      if (!(ptr = strpbrk(ptr, ": "))) goto leave;
      *ptr++ = 0;

      char* const iv = ptr;
      if (strcmp(crypt_algorythm, "NONE")) {
        if (!(ptr = strchr(ptr, ' '))) goto leave;
        *ptr++ = 0;
      }
      ptr = (char*) skipsp(ptr);

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
        DES_key_schedule schedule;
        DES_set_key_unchecked((const_DES_cblock*) &digest, &schedule);
        DES_ncbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &schedule, (const_DES_cblock*) &iv, DES_DECRYPT);
      }
      else if (!strcmp(crypt_algorythm, "3DES")) {
        DES_key_schedule schedule1, schedule2, schedule3;
        DES_set_key_unchecked((const_DES_cblock*) (digest+ 0), &schedule1);
        DES_set_key_unchecked((const_DES_cblock*) (digest+ 8), &schedule2);
        DES_set_key_unchecked((const_DES_cblock*) (digest+16), &schedule3);
        DES_ede3_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &schedule1, &schedule2, &schedule3,
            (const_DES_cblock*) &iv, DES_DECRYPT);
      } else {
        data = strdup(ptr);
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
        if (colon) {
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
        gboolean notification_sticky = FALSE;
        char* notification_display_name = NULL;
        while (*ptr) {
          char* const line = ptr;
          ptr = crlf_to_term_and_skip(ptr);
          if (*line== '\0') break;
          cr_to_lf(line);

          const char* const colon = strchr(line, ':');
          if (colon) {
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
            else if (!strncmp(line, "Notification-Sticky:", 20)) {
              notification_sticky = strcasecmp(value, "true") == 0;
            }
            g_free(value);
          }
        }

        int exist = 0;
        void
        get_count(sqlite3_stmt* const stmt) {
          exist = sqlite3_step(stmt) == SQLITE_ROW
            ? (gboolean) sqlite3_column_int(stmt, 0)
            : 0;
        }
        statement_sqlite3(get_count,
          "select count(*) from application where app_name = '%q' and name = '%q'",
          application_name, notification_name);

        if (!exist) {
          exec_sqlite3(
            "delete from application where app_name = '%q' and name = '%q'",
            application_name, notification_name);

          exec_sqlite3(
            "insert into application("
            "app_name, app_icon, name, icon, enable, display, sticky)"
            " values('%q', '%q', '%q', '%q', %d, '%q', %d)",
            application_name,
            application_icon ? application_icon : "",
            notification_name,
            notification_icon ? notification_icon : "",
            notification_enabled,
            notification_display_name ?
              notification_display_name : "Fog",
            notification_sticky);
        }

        g_free(notification_name);
        g_free(notification_icon);
        g_free(notification_display_name);
      }
      parse_identifiers(ptr);

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
        if (colon) {
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
          else if (!strncmp(line, "Notification-Sticky:", 20)) {
            ni->sticky = strcasecmp(value, "true") == 0;
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
      parse_identifiers(ptr);

      exec_sqlite3(
        "insert into notification("
        "title, text, icon, url, received)"
        " values('%q', '%q', '%q', '%q', current_timestamp)",
        ni->title, ni->text,
        ni->icon ? ni->icon : "",
        ni->url ? ni->url : "");
      if (setting_dialog) {
        GtkTreeModel* const model
          = (GtkTreeModel*) get_data_as_object(setting_dialog, "notifications");
        gchar* value;
        void
        get_current_timestamp(sqlite3_stmt* const stmt) {
          value = g_strdup(
              sqlite3_step(stmt) == SQLITE_ROW
                ? (char*) sqlite3_column_text(stmt, 0)
                : "");
        }
        statement_sqlite3(get_current_timestamp, "select current_timestamp");
        list_store_set_before_prepand(GTK_LIST_STORE(model),
            0, value,
            1, ni->title,
            2, ni->text,
            -1);
        g_free(value);
      }

      raise_notification(
        (CLIENT_INFO){
          .sock                      = sock,
          .command                   = command,
          .application_name          = application_name,
          .notification_name         = notification_name,
          .notification_display_name = notification_display_name,
        }, ni);

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

#ifdef HAVE_APP_INDICATOR
static void
gol_status_toggle(GtkMenuItem* menu_item, gpointer GOL_UNUSED_ARG(user_data)) {
  switch (gol_status)
  {
    case GOL_STATUS_NORMAL:
      gol_status = GOL_STATUS_DND;
      app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
      gtk_menu_item_set_label(menu_item, "Switch to on");
      break;
    case GOL_STATUS_DND:
      gol_status = GOL_STATUS_NORMAL;
      app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
      gtk_menu_item_set_label(menu_item, "Switch to off");
      break;
  }
}
static void
create_menu() {
  gchar* theme_path;
  GtkWidget* menu_item;

  indicator = app_indicator_new("Growl", "icon_dnd",
                                 APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  theme_path = g_build_filename(DATADIR, "data", NULL);
  app_indicator_set_icon_theme_path(indicator, theme_path);
  g_free(theme_path);

  popup_menu = gtk_menu_new();
  menu_item = gtk_menu_item_new_with_label("Switch to off");
  gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), menu_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu),
    gtk_separator_menu_item_new());
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_PREFERENCES, G_CALLBACK(settings_clicked));
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_ABOUT, G_CALLBACK(about_click));
  gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu),
    gtk_separator_menu_item_new());
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_QUIT, G_CALLBACK(exit_clicked));
  app_indicator_set_menu(indicator, GTK_MENU(popup_menu));

  g_signal_connect(G_OBJECT(menu_item), "activate",
    G_CALLBACK(gol_status_toggle), NULL);

  app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
  app_indicator_set_icon_full(indicator, "icon_dnd", NULL);
  app_indicator_set_attention_icon_full(indicator, "icon", NULL);
  gtk_widget_show_all(popup_menu);
}
#else
static gboolean
gol_status_toggle(GtkStatusIcon* status_icon, GdkEvent* event, gpointer user_data) {
  switch (gol_status)
  {
    case GOL_STATUS_NORMAL:
      gol_status = GOL_STATUS_DND;
      gtk_status_icon_set_from_pixbuf(status_icon, status_icon_dnd_pixbuf);
      break;
    case GOL_STATUS_DND:
      gol_status = GOL_STATUS_NORMAL;
      gtk_status_icon_set_from_pixbuf(status_icon, status_icon_pixbuf);
      break;
  }
  return FALSE;
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

static void
create_menu() {
  status_icon = gtk_status_icon_new();
  gtk_status_icon_set_tooltip(status_icon, "Growl");

  // TODO: absolute path
  status_icon_pixbuf = pixbuf_from_datadir("icon.png", NULL);
  status_icon_dnd_pixbuf = pixbuf_from_datadir("icon_dnd.png", NULL);

  gtk_status_icon_set_from_pixbuf(status_icon, status_icon_pixbuf);

  popup_menu = gtk_menu_new();
  g_signal_connect(G_OBJECT(status_icon), "popup-menu",
      G_CALLBACK(status_icon_popup), popup_menu);

  g_signal_connect(G_OBJECT(status_icon), "button-release-event",
      G_CALLBACK(gol_status_toggle), 0);

  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_PREFERENCES, G_CALLBACK(settings_clicked));
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_ABOUT, G_CALLBACK(about_click));
  gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu),
    gtk_separator_menu_item_new());
  append_new_menu_item_from_stock(GTK_MENU_SHELL(popup_menu),
    GTK_STOCK_QUIT, G_CALLBACK(exit_clicked));

  gtk_widget_show_all(popup_menu);
  gtk_status_icon_set_visible(status_icon, TRUE);
}
#endif

static void
destroy_menu() {
  if (popup_menu) {
      gtk_widget_destroy(popup_menu);
  }
#ifndef HAVE_APP_INDICATOR
  if (status_icon) {
      gtk_status_icon_set_visible(GTK_STATUS_ICON(status_icon), FALSE);
      g_object_unref(G_OBJECT(status_icon));
  }
#endif
  if (status_icon_pixbuf) {
    g_object_unref(G_OBJECT(status_icon_pixbuf));
  }
  if (status_icon_dnd_pixbuf) {
    g_object_unref(G_OBJECT(status_icon_dnd_pixbuf));
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
      "drop table _application",
      "drop table _subscriber",
      "drop table _display",
      "alter table application rename to _application",
      "alter table subscriber rename to _subscriber",
      "alter table display rename to _display",
      "create table notification("
          "title text not null,"
          "text text not null,"
          "icon text,"
          "url text,"
          "received timestamp not null)",
      "create table application("
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
      "create table display("
          "name text not null primary key,"
          "parameter text)",
      "insert into notification from select * from _notification",
      "insert into application from select * from _application",
      "insert into subscriber from select * from _subscriber",
      "insert into display from select * from _display",
      "drop table _notification",
      "drop table _application",
      "drop table _subscriber",
      "drop table _display",
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

static gboolean
load_display_plugins() {
  gchar* const path = g_build_filename(LIBDIR, "display", NULL);
  GDir* const dir = g_dir_open(path, 0, NULL);
  if (!dir) {
    perror("open");
    g_critical("Display plugin directory isn't found: %s", path);
    return FALSE;
  }

  gchar* const default_display = get_config_string("default_display", "Fog");

  current_display = NULL;
  const gchar *filename;
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* const filepath = g_build_filename(path, filename, NULL);
    GModule* const handle = g_module_open(filepath, G_MODULE_BIND_LAZY);
    g_free(filepath);
    if (!handle) {
      continue;
    }
    DISPLAY_PLUGIN* const dp = g_new0(DISPLAY_PLUGIN, 1);
    if (!dp) {
      g_critical("fatal: g_new0(DISPLAY_PLUGIN, 1) failed");
      g_module_close(handle);
      unload_display_plugins();
      break;
    }
    dp->handle = handle;
    g_module_symbol(handle, "display_show", (void**) &dp->show);
    g_module_symbol(handle, "display_init", (void**) &dp->init);
    g_module_symbol(handle, "display_term", (void**) &dp->term);
    g_module_symbol(handle, "display_name", (void**) &dp->name);
    g_module_symbol(handle, "display_description", (void**) &dp->description);
    g_module_symbol(handle, "display_thumbnail", (void**) &dp->thumbnail);
    g_module_symbol(handle, "display_set_param", (void**) &dp->set_param);
    g_module_symbol(handle, "display_get_param", (void**) &dp->get_param);
    const char* const name = dp->name ? dp->name() : NULL;
    if (name && dp->init && !dp->init()) {
      g_module_close(dp->handle);
      g_free(dp);
      continue;
    }

    display_plugins = g_list_append(display_plugins, dp);
    gol_debug_message("load display plugins \"%s\".", name);

    char* const sql = sqlite3_mprintf("select name, value from display where name = '%q'", name);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
    if (sqlite3_column_text(stmt, 0)) {
      if (dp->set_param) dp->set_param((const char*) sqlite3_column_text(stmt, 1));
    }
    sqlite3_finalize(stmt);

    if (!g_ascii_strcasecmp(name, default_display))
      current_display = dp;
  }

  g_dir_close(dir);
  g_free(path);
  g_free(default_display);

  if (g_list_length(display_plugins) == 0) {
    g_critical("No display plugins found.");
    return FALSE;
  }

  if (!current_display) current_display = g_list_nth_data(display_plugins, 0);
  if (current_display->set_param)
    current_display->set_param(get_display_parameter(current_display->name(), ""));

  return TRUE;
}

static void
subscribe_show(NOTIFICATION_INFO* const ni) {
  ni->timeout = get_config_value("default_timeout", 5000)/10;
  g_idle_add((GSourceFunc) current_display->show, ni);
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

    gchar* const filepath = g_build_filename(path, filename, NULL);
    GModule* const handle = g_module_open(filepath, G_MODULE_BIND_LAZY);
    g_free(filepath);
    if (!handle) continue;
    SUBSCRIBE_PLUGIN* const sp = g_new0(SUBSCRIBE_PLUGIN, 1);
    if (!sp) {
      g_critical("fatal: g_new0(SUBSCRIBE_PLUGIN, 1) failed");
      g_module_close(handle);
      unload_subscribe_plugins();
      break;
    }
    sp->handle = handle;
    g_module_symbol(handle, "subscribe_start", (void**) &sp->start);
    g_module_symbol(handle, "subscribe_stop", (void**) &sp->stop);
    g_module_symbol(handle, "subscribe_init", (void**) &sp->init);
    g_module_symbol(handle, "subscribe_term", (void**) &sp->term);
    g_module_symbol(handle, "subscribe_name", (void**) &sp->name);
    g_module_symbol(handle, "subscribe_description", (void**) &sp->description);
    g_module_symbol(handle, "subscribe_thumbnail", (void**) &sp->thumbnail);
    const char* const name = sp->name ? sp->name() : NULL;
    if (name && sp->init && !sp->init(&sc)) {
      g_module_close(sp->handle);
      g_free(sp);
      continue;
    }

    subscribe_plugins = g_list_append(subscribe_plugins, sp);
    gol_debug_message("load subscriber plugins \"%s\".", name);
    if (get_subscriber_enabled(name)) sp->start();
  }

  g_dir_close(dir);
  g_free(path);

  return TRUE;
}

static gboolean
gntp_accepted(GIOChannel* const source, GIOCondition GOL_UNUSED_ARG(condition), gpointer GOL_UNUSED_ARG(user_data)) {
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
# if !GLIB_CHECK_VERSION(2, 32, 0)
  g_thread_create(gntp_recv_proc, (gpointer)(intptr_t) sock, FALSE, NULL);
# else
  GThread *tid = g_thread_try_new("accept", gntp_recv_proc, (gpointer)(intptr_t) sock, NULL);
  g_thread_unref(tid); // detach immediately
# endif
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
udp_recv_proc(GIOChannel* const source, GIOCondition GOL_UNUSED_ARG(condition), gpointer GOL_UNUSED_ARG(user_data)) {
  int is_local_app = FALSE;
  int fd = g_io_channel_unix_get_fd(source);
  char buf[BUFSIZ] = {0};
  struct sockaddr_in client;
  socklen_t client_len = sizeof(client);
  memset(&client, 0, sizeof(client));

  if (!getsockname(fd, (struct sockaddr *) &client, &client_len)) {
    const char* addr = inet_ntoa(((struct sockaddr_in *)(void*)&client)->sin_addr);
    if (addr && !strcmp(addr, "127.0.0.1")) {
      is_local_app = TRUE;
    }
  }
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
  unsigned char digest[GOL_PP_CAT(_hash_algorithm, _DIGEST_LENGTH)] = {0}; \
  const size_t datalen = _datalen - sizeof(digest);\
  GOL_PP_CAT(_hash_algorithm, _CTX) ctx;\
  GOL_PP_CAT(_hash_algorithm, _Init)(&ctx);\
  GOL_PP_CAT(_hash_algorithm, _Update)(&ctx, _data, datalen);\
  GOL_PP_CAT(_hash_algorithm, _Update)(&ctx, _password, strlen(_password));\
  GOL_PP_CAT(_hash_algorithm, _Final)(digest, &ctx);\
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
        ni->timeout = get_config_value("default_timeout", 5000)/10;
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

  const struct sockaddr_in server_addr = {
    .sin_family      = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_ANY),
    .sin_port        = htons(9887),
  };

  if (bind(fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    return NULL;
  }

  fd_set fdset;
  FD_ZERO(&fdset);
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

  const struct sockaddr_in server_addr = {
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
  FD_ZERO(&fdset);
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

static void
usage(void) {
  fprintf(stderr,"Usage: gol [option]\n");
  fprintf(stderr," -h    : show this help\n");
  exit(1);
}

int
main(int argc, char* argv[]) {
  int ch;

  gchar* program = g_find_program_in_path(argv[0]);
  exepath = g_path_get_dirname(program);
  g_free(program);

  while ((ch = getopt(argc, argv, "h")) != -1) {
    switch (ch){
    case 'h':
      usage();
      break;
    default:
      usage();
    }
  }
  argc -= optind;
  argv += optind;

#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
  GIOChannel* gntp_io = NULL;
  GIOChannel* udp_io = NULL;

#ifdef G_THREADS_ENABLED
#if !GLIB_CHECK_VERSION(2,23,2)
  g_thread_init(NULL);
#endif
#endif

  g_set_application_name("growl-for-linux");

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

