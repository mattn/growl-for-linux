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
#include <ctype.h>
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

static long
read_all(int fd, char** ptr) {
  int i = 0, r;
  *ptr = (char*) calloc(BUFSIZ + 1, 1);
  while (*ptr && (r = recv(fd, *ptr + i, BUFSIZ, 0)) > 0) {
    i += r;
    if (r > 2 && !strncmp(*ptr + i - 4, "\r\n\r\n", 4)) break;
    *ptr = realloc(*ptr, BUFSIZ + i + 1);
  }
  *(*ptr+i) = 0;
  return i;
}

unsigned int
unhex(unsigned char c) {
  if('0' <= c && c <= '9') return (c - '0');
  if('a' <= c && c <= 'f') return (0x0a + c - 'a');
  if('A' <= c && c <= 'F') return (0x0a + c - 'A');
  return 0;
}

static gboolean
get_config_bool(const char* key, gboolean def) {
  const char* sql = sqlite3_mprintf(
      "select value from config where key = '%q'", key);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  gboolean ret = def;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ret = (gboolean) sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_free((void*) sql);
  return ret;
}

static gboolean
get_subscriber_enabled(const char* name) {
  const char* sql = sqlite3_mprintf(
      "select enable from subscriber where name = '%q'", name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  gboolean ret = FALSE;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ret = (gboolean) sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_free((void*) sql);
  return ret;
}

static gchar*
get_config_string(const char* key, const char* def) {
  const char* sql = sqlite3_mprintf(
      "select value from config where key = '%q'", key);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  gchar* value = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    value = g_strdup((char*) sqlite3_column_text(stmt, 0));
  } else {
    value = g_strdup(def ? def : "");
  }
  sqlite3_finalize(stmt);
  sqlite3_free((void*) sql);
  return value;
}

static void
set_config_bool(const char* key, gboolean value) {
  const char* sql;
  sql = sqlite3_mprintf(
        "delete from config where key = '%q'", key);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);
  sql = sqlite3_mprintf(
        "insert into config(key, value) values('%q', '%q')",
          key, value ? "1" : "0");
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);
}

static void
set_config_string(const char* key, const char* value) {
  const char* sql;
  sql = sqlite3_mprintf(
        "delete from config where key = '%q'", key);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);
  sql = sqlite3_mprintf(
        "insert into config(key, value) values('%q', '%q')", key, value);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);
}

static void
my_gtk_status_icon_position_menu(
    GtkMenu* menu, gint* x, gint* y, gboolean* push_in, gpointer data) {
  gtk_status_icon_position_menu(menu, x, y, push_in, data);
#ifdef _WIN32
  RECT rect;
  SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);
  gint h = GTK_WIDGET(menu)->requisition.height;
  if (*y + h > rect.bottom) *y -= h;
#endif
}

static void
status_icon_popup(
    GtkStatusIcon* status_icon, guint button,
    guint32 activate_time, gpointer menu) {
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
      my_gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static void
display_tree_selection_changed(GtkTreeSelection *selection, gpointer data) {
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  DISPLAY_PLUGIN* cp = current_display;
  gchar* name;
  gtk_tree_model_get(model, &iter, 0, &name, -1);
  int i, len = g_list_length(display_plugins);
  for (i = 0; i < len; i++) {
    DISPLAY_PLUGIN* dp = (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
    if (!g_strcasecmp(dp->name(), name)) {
      cp = dp;
      break;
    }
  }

  GtkWidget* label =
    (GtkWidget*) g_object_get_data(G_OBJECT(data), "description");
  gtk_label_set_markup(GTK_LABEL(label), "");
  if (cp->description) {
    gtk_label_set_markup(GTK_LABEL(label), cp->description());
  }

  GtkWidget* image =
    (GtkWidget*) g_object_get_data(G_OBJECT(data), "thumbnail");
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
application_tree_selection_changed(GtkTreeSelection *selection, gpointer data) {
  GtkTreeIter iter;
  GtkTreeModel* model1;
  if (!gtk_tree_selection_get_selected(selection, &model1, &iter)) return;

  gchar* app_name;
  gtk_tree_model_get(model1, &iter, 0, &app_name, -1);

  GtkListStore* model2 =
    (GtkListStore*) g_object_get_data(G_OBJECT(data), "model2");
  gtk_list_store_clear(model2);
  const char* sql = sqlite3_mprintf(
      "select distinct name from notification"
      " where app_name = '%q' order by name", app_name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    GtkTreeIter iter;
    gtk_list_store_append(GTK_LIST_STORE(model2), &iter);
    gtk_list_store_set(GTK_LIST_STORE(model2), &iter, 0,
        sqlite3_column_text(stmt, 0), -1);
  }
  sqlite3_finalize(stmt);
  sqlite3_free((void*) sql);

  g_free(app_name);

  gtk_widget_set_sensitive(
    (GtkWidget*) g_object_get_data(G_OBJECT(data), "enable"), FALSE);
  gtk_widget_set_sensitive(
    (GtkWidget*) g_object_get_data(G_OBJECT(data), "display"), FALSE);
}

static void
set_as_default_clicked(GtkWidget* widget, gpointer data) {
  GtkTreeSelection* selection = (GtkTreeSelection*) data;
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    gchar* name;
    gtk_tree_model_get(model, &iter, 0, &name, -1);
    int i, len = g_list_length(display_plugins);
    for (i = 0; i < len; i++) {
      DISPLAY_PLUGIN* dp =
        (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      if (!g_strcasecmp(dp->name(), name)) {
        current_display = dp;
        set_config_string("default_display", name);
        break;
      }
    }
    g_free(name);
  }
}

static void
preview_clicked(GtkWidget* widget, gpointer data) {
  GtkTreeSelection* selection = (GtkTreeSelection*) data;
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    gchar* name;
    gtk_tree_model_get(model, &iter, 0, &name, -1);
    int i, len = g_list_length(display_plugins);
    for (i = 0; i < len; i++) {
      DISPLAY_PLUGIN* dp =
        (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      if (!g_strcasecmp(dp->name(), name)) {
        NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
        ni->title = g_strdup("Preview Display");
        ni->text = g_strdup_printf(
            "This is a preview of the '%s' display.", dp->name());
        ni->icon = g_build_filename(DATADIR, "data", "mattn.png", NULL);
        ni->local = TRUE;
        g_idle_add((GSourceFunc) dp->show, ni);
        break;
      }
    }
    g_free(name);
  }
}

static gboolean
password_focus_out(GtkWidget* widget, GdkEvent* event, gpointer data) {
  g_free(password);
  password = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
  set_config_string("password", password);
  return FALSE;
}

static void
require_password_for_local_apps_changed(
    GtkToggleButton *togglebutton, gpointer data) {
  require_password_for_local_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_local_apps",
      require_password_for_local_apps);
}

static void
require_password_for_lan_apps_changed(
    GtkToggleButton *togglebutton, gpointer data) {
  require_password_for_lan_apps
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));
  set_config_bool("require_password_for_lan_apps",
      require_password_for_lan_apps);
}

static void
subscriber_enable_toggled(
    GtkCellRendererToggle *cell, gchar* path_str, gpointer data) {

  GtkTreeModel* model = (GtkTreeModel *)data;
  GtkTreeIter iter;
  GtkTreePath* path = gtk_tree_path_new_from_string (path_str);
  gboolean enable;
  gchar* name;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter, 0, &enable, 1, &name, -1);
  enable ^= 1;
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, enable, -1);

  const char* sql;
  sql = sqlite3_mprintf(
        "delete from subscriber where name = '%q'", name);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);
  sql = sqlite3_mprintf(
        "insert into subscriber(name, enable) values('%q', %d)",
          name, enable ? 1 : 0);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);

  int i, len = g_list_length(subscribe_plugins);
  for (i = 0; i < len; i++) {
    SUBSCRIBE_PLUGIN* sp =
      (SUBSCRIBE_PLUGIN*) g_list_nth_data(subscribe_plugins, i);
    if (!g_strcasecmp(sp->name(), name)) {
      enable ? sp->start() : sp->stop();
      break;
    }
  }

  g_free(name);

  gtk_tree_path_free(path);
}

static void
notification_tree_selection_changed(GtkTreeSelection *selection, gpointer data) {
  GtkTreeIter iter1;
  GtkTreeIter iter2;
  GtkWidget* tree1 = g_object_get_data(G_OBJECT(data), "tree1");
  GtkWidget* tree2 = g_object_get_data(G_OBJECT(data), "tree2");
  GtkTreeSelection* selection1
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree1));
  GtkTreeSelection* selection2
    = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree2));
  GtkTreeModel* model1;
  GtkTreeModel* model2;

  if (!gtk_tree_selection_get_selected(selection1, &model1, &iter1)) return;
  if (!gtk_tree_selection_get_selected(selection2, &model2, &iter2)) return;

  gchar* app_name;
  gchar* name;
  gtk_tree_model_get(model1, &iter1, 0, &app_name, -1);
  gtk_tree_model_get(model2, &iter2, 0, &name, -1);

  GtkWidget* combobox1
    = (GtkWidget*) g_object_get_data(G_OBJECT(data), "enable");
  gtk_widget_set_sensitive(combobox1, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(combobox1), -1);
  GtkWidget* combobox2
    = (GtkWidget*) g_object_get_data(G_OBJECT(data), "display");
  gtk_widget_set_sensitive(combobox2, TRUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(combobox2), -1);

  const char* sql = sqlite3_mprintf(
      "select enable, display from notification"
      " where app_name = '%q' and name = '%q'", app_name, name);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(combobox1),
        sqlite3_column_int(stmt, 0) != 0 ? 0 : 1);
    char* display = (char*) sqlite3_column_text(stmt, 1);
    int i, len = g_list_length(display_plugins);
    for (i = 0; i < len; i++) {
      DISPLAY_PLUGIN* dp
        = (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      if (!g_strcasecmp(dp->name(), display)) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combobox2), i);
        break;
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_free((void*) sql);

  g_free(app_name);
  g_free(name);
}

static void
notification_enable_changed(GtkComboBox *combobox, gpointer data) {
  GtkTreeIter iter1;
  GtkTreeIter iter2;
  GtkWidget* tree1 = g_object_get_data(G_OBJECT(data), "tree1");
  GtkWidget* tree2 = g_object_get_data(G_OBJECT(data), "tree2");
  GtkTreeSelection* selection1 =
    gtk_tree_view_get_selection(GTK_TREE_VIEW(tree1));
  GtkTreeSelection* selection2 =
    gtk_tree_view_get_selection(GTK_TREE_VIEW(tree2));
  GtkTreeModel* model1;
  GtkTreeModel* model2;

  if (!gtk_tree_selection_get_selected(selection1, &model1, &iter1)) return;
  if (!gtk_tree_selection_get_selected(selection2, &model2, &iter2)) return;

  gchar* app_name;
  gchar* name;
  gtk_tree_model_get(model1, &iter1, 0, &app_name, -1);
  gtk_tree_model_get(model2, &iter2, 0, &name, -1);

  gint enable = gtk_combo_box_get_active(combobox) == 0 ? 1 : 0;

  const char* sql = sqlite3_mprintf(
        "update notification set enable = %d"
        " where app_name = '%q' and name = '%q'",
        enable, app_name, name);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);

  g_free(app_name);
  g_free(name);
}

static void
notification_display_changed(GtkComboBox *combobox, gpointer data) {
  GtkTreeIter iter1;
  GtkTreeIter iter2;
  GtkWidget* tree1 = g_object_get_data(G_OBJECT(data), "tree1");
  GtkWidget* tree2 = g_object_get_data(G_OBJECT(data), "tree2");
  GtkTreeSelection* selection1 =
    gtk_tree_view_get_selection(GTK_TREE_VIEW(tree1));
  GtkTreeSelection* selection2 =
    gtk_tree_view_get_selection(GTK_TREE_VIEW(tree2));
  GtkTreeModel* model1;
  GtkTreeModel* model2;

  if (!gtk_tree_selection_get_selected(selection1, &model1, &iter1)) return;
  if (!gtk_tree_selection_get_selected(selection2, &model2, &iter2)) return;

  gchar* app_name;
  gchar* name;
  gtk_tree_model_get(model1, &iter1, 0, &app_name, -1);
  gtk_tree_model_get(model2, &iter2, 0, &name, -1);

  gchar* display = gtk_combo_box_get_active_text(combobox);

  const char* sql = sqlite3_mprintf(
        "update notification set display = '%q'"
        " where app_name = '%q' and name = '%q'",
        display, app_name, name);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free((void*) sql);

  g_free(display);
  g_free(app_name);
  g_free(name);
}

static void
settings_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  GtkWidget* dialog;
  GtkWidget* notebook;

  dialog = gtk_dialog_new_with_buttons(
      "Settings", NULL, GTK_DIALOG_MODAL,
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
  gchar* path = g_build_filename(DATADIR, "data", "icon.png", NULL);
  gtk_window_set_icon_from_file(GTK_WINDOW(dialog), path, NULL);
  g_free(path);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);

  notebook = gtk_notebook_new();
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), notebook);
  
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
        G_CALLBACK(display_tree_selection_changed), dialog);
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
    g_object_set_data(G_OBJECT(dialog), "description", label);
    GtkWidget* align = gtk_alignment_new(0, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(align), label);
    gtk_box_pack_start(GTK_BOX(vbox), align, FALSE, FALSE, 0);
    GtkWidget* image = gtk_image_new();
    g_object_set_data(G_OBJECT(dialog), "thumbnail", image);
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

    int i, len = g_list_length(display_plugins);
    for (i = 0; i < len; i++) {
      GtkTreeIter iter;
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      DISPLAY_PLUGIN* dp =
        (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, dp->name(), -1);
      if (dp == current_display)
        gtk_tree_selection_select_iter(select, &iter);
    }
  }

  {
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
        hbox, gtk_label_new("Application"));

    GtkListStore* model1 =
      (GtkListStore *) gtk_list_store_new(1, G_TYPE_STRING);
    g_object_set_data(G_OBJECT(dialog), "model1", model1);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model1));
    g_object_set_data(G_OBJECT(dialog), "tree1", tree_view);
    GtkTreeSelection* select
      = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
        G_CALLBACK(application_tree_selection_changed), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), tree_view, FALSE, FALSE, 0);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
        "Application", gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_column_set_min_width(GTK_TREE_VIEW_COLUMN(column), 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    GtkListStore* model2 =
      (GtkListStore *) gtk_list_store_new(1, G_TYPE_STRING);
    g_object_set_data(G_OBJECT(dialog), "model2", model2);
    tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model2));
    g_object_set_data(G_OBJECT(dialog), "tree2", tree_view);
    select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
        G_CALLBACK(notification_tree_selection_changed), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), tree_view, FALSE, FALSE, 0);
    column = gtk_tree_view_column_new_with_attributes(
        "Notification", gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_column_set_min_width(GTK_TREE_VIEW_COLUMN(column), 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    GtkWidget* frame = gtk_frame_new("Setting");
    gtk_box_pack_start(GTK_BOX(hbox), frame, TRUE, TRUE, 0);

    GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget* label = gtk_label_new("Enable:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    GtkWidget* combobox = gtk_combo_box_new_text();
    g_object_set_data(G_OBJECT(dialog), "enable", combobox);
    gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), "Enable");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), "Disable");
    gtk_widget_set_sensitive(combobox, FALSE);
    g_signal_connect(G_OBJECT(combobox), "changed",
        G_CALLBACK(notification_enable_changed), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), combobox, FALSE, FALSE, 0);

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    label = gtk_label_new("Display:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    combobox = gtk_combo_box_new_text();
    g_object_set_data(G_OBJECT(dialog), "display", combobox);
    gtk_widget_set_sensitive(combobox, FALSE);
    g_signal_connect(G_OBJECT(combobox), "changed",
        G_CALLBACK(notification_display_changed), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), combobox, FALSE, FALSE, 0);

    const char* sql;
    sqlite3_stmt *stmt = NULL;

    sql = "select distinct app_name from notification order by app_name";
    sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      GtkTreeIter iter;
      gtk_list_store_append(GTK_LIST_STORE(model1), &iter);
      gtk_list_store_set(GTK_LIST_STORE(model1), &iter, 0,
          sqlite3_column_text(stmt, 0), -1);
    }
    sqlite3_finalize(stmt);

    int i, len = g_list_length(display_plugins);
    for (i = 0; i < len; i++) {
      DISPLAY_PLUGIN* dp =
        (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
      gtk_combo_box_append_text(GTK_COMBO_BOX(combobox), dp->name());
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

    int i, len = g_list_length(subscribe_plugins);
    for (i = 0; i < len; i++) {
      GtkTreeIter iter;
      SUBSCRIBE_PLUGIN* sp =
        (SUBSCRIBE_PLUGIN*) g_list_nth_data(subscribe_plugins, i);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter,
          0, get_subscriber_enabled(sp->name()),
          1, sp->name(),
          2, sp->description(),
          -1);
    }
  }

  gtk_widget_set_size_request(dialog, 500, 500);
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void
about_click(GtkWidget* widget, gpointer user_data) {
  const gchar* authors[2] = {"mattn <mattn.jp@gmail.com>", NULL};
  gchar* contents = NULL;
  gchar* utf8 = NULL;
  GdkPixbuf* logo = NULL;
  GtkWidget* dialog;
  dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), "Growl For Linux");
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
		  "https://github.com/mattn/growl-for-linux/");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
		  "A notification system for linux");
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), PACKAGE_VERSION);
  if (g_file_get_contents("COPYING", &contents, NULL, NULL)) {
    utf8 = g_locale_to_utf8(contents, -1, NULL, NULL, NULL);
    g_free(contents);
    contents = NULL;
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog), utf8);
    g_free(utf8);
    utf8 = NULL;
  }
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
      "http://mattn.kaoriya.net/");
  gchar* path = g_build_filename(DATADIR, "data", NULL);
  gchar* fullpath = g_build_filename(path, "growl4linux.jpg", NULL);
  g_free(path);
  logo = gdk_pixbuf_new_from_file(fullpath, NULL);
  g_free(fullpath);
  gtk_about_dialog_set_logo (GTK_ABOUT_DIALOG(dialog), logo);
  g_object_unref(G_OBJECT(logo));
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void
exit_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  gtk_main_quit();
}

static gpointer
gntp_recv_proc(gpointer data) {
  int sock = (int) data;
  int is_local_app = FALSE;

  struct sockaddr_in client;
  int client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if (!getsockname(sock, (struct sockaddr *) &client,
        (socklen_t *) &client_len)) {
    char* addr = inet_ntoa(((struct sockaddr_in *)(void*)&client)->sin_addr);
    if (addr && !strcmp(addr, "127.0.0.1")) {
      is_local_app = TRUE;
    }
  }

  char* ptr;
  int r = read_all(sock, &ptr);
  char* top = ptr;
  if (!strncmp(ptr, "GNTP/1.0 ", 9)) {
    ptr += 9;

    char* command = ptr;
    if (!strncmp(ptr, "REGISTER ", 9)) ptr += 8;
    else if (!strncmp(ptr, "NOTIFY ", 7)) ptr += 6;
    else goto leave;

    *ptr++ = 0;

    char* data = NULL;
    if (!strncmp(ptr, "NONE", 4) && strchr("\n ", *(ptr+5))) {
      if (is_local_app && get_config_bool(
            "require_password_for_local_apps", FALSE)) goto leave;
      if (!is_local_app && get_config_bool(
            "require_password_for_lan_apps", FALSE)) goto leave;
      if (!(ptr = strstr(ptr, "\r\n"))) goto leave;
      *ptr = 0;
      ptr += 2;
      data = (char*) calloc(r-(ptr-top)-4+1, 1);
      if (!data) goto leave;
      memcpy(data, ptr, r-(ptr-top)-4);
    } else {
      if (strncmp(ptr, "AES:", 4) &&
          strncmp(ptr, "DES:", 4) &&
          strncmp(ptr, "3DES:", 5)) goto leave;

      char* crypt_algorythm = ptr;
      while (*ptr != ':') ptr++;
      *ptr++ = 0;
      char* iv;
      iv = ptr;
      if (!(ptr = strchr(ptr, ' '))) goto leave;
      *ptr++ = 0;

      if (strncmp(ptr, "MD5:", 4) &&
          strncmp(ptr, "SHA1:", 5) &&
          strncmp(ptr, "SHA256:", 7)) goto leave;

      char* hash_algorythm = ptr;
      while (*ptr != ':') ptr++;
      *ptr++ = 0;
      char* key = ptr;
      if (!(ptr = strchr(ptr, '.'))) goto leave;
      *ptr++ = 0;
      char* salt = ptr;

      if (!(ptr = strstr(ptr, "\r\n"))) goto leave;
      *ptr = 0;
      ptr += 2;

      int n, keylen, saltlen, ivlen;

      char hex[3];
      hex[2] = 0;
      saltlen = strlen(salt) / 2;
      for (n = 0; n < saltlen; n++)
        salt[n] = unhex(salt[n * 2]) * 16 + unhex(salt[n * 2 + 1]);
      keylen = strlen(key) / 2;
      for (n = 0; n < keylen; n++)
        key[n] = unhex(key[n * 2]) * 16 + unhex(key[n * 2 + 1]);
      ivlen = strlen(iv) / 2;
      for (n = 0; n < ivlen; n++)
        iv[n] = unhex(iv[n * 2]) * 16 + unhex(iv[n * 2 + 1]);

      char digest[32] = {0};
      memset(digest, 0, sizeof(digest));

      if (!strcmp(hash_algorythm, "MD5")) {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        MD5_Update(&ctx, password, strlen(password));
        MD5_Update(&ctx, salt, saltlen);
        MD5_Final((unsigned char*) digest, &ctx);
      }
      if (!strcmp(hash_algorythm, "SHA1")) {
        SHA_CTX ctx;
        SHA1_Init(&ctx);
        SHA1_Update(&ctx, password, strlen(password));
        SHA1_Update(&ctx, salt, saltlen);
        SHA1_Final((unsigned char*) digest, &ctx);
      }
      if (!strcmp(hash_algorythm, "SHA256")) {
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, password, strlen(password));
        SHA256_Update(&ctx, salt, saltlen);
        SHA256_Final((unsigned char*) digest, &ctx);
      }

      data = (char*) calloc(r, 1);
      if (!data) goto leave;
      if (!strcmp(crypt_algorythm, "AES")) {
        AES_KEY aeskey;
        AES_set_decrypt_key((unsigned char*) digest, 24 * 8, &aeskey);
        AES_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &aeskey, (unsigned char*) iv, AES_DECRYPT);
      }
      if (!strcmp(crypt_algorythm, "DES")) {
        des_key_schedule schedule;
        DES_set_key_unchecked((const_DES_cblock*) &digest, &schedule);
        DES_ncbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, &schedule, (const_DES_cblock*) &iv, DES_DECRYPT);
      }
      if (!strcmp(crypt_algorythm, "3DES")) {
        char key1[8], key2[8], key3[8];
        memcpy(key1, digest+ 0, 8);
        memcpy(key2, digest+ 8, 8);
        memcpy(key3, digest+16, 8);
        des_key_schedule schedule1, schedule2, schedule3;
        DES_set_key_unchecked((const_DES_cblock*) &key1, &schedule1);
        DES_set_key_unchecked((const_DES_cblock*) &key2, &schedule2);
        DES_set_key_unchecked((const_DES_cblock*) &key3, &schedule3);
        des_ede3_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
            r-(ptr-top)-6, schedule1, schedule2, schedule3,
            (const_DES_cblock*) &iv, DES_DECRYPT);
      }
    }

    ptr = data;
    if (!strcmp(command, "REGISTER")) {
      char* application_name = NULL;
      char* application_icon = NULL;
      int notifications_count = 0;
      while (*ptr) {
        char* line = ptr;
        while (*ptr) {
          if (*ptr == '\r') {
            if (*(ptr+1) == '\n') {
              *ptr = 0;
              ptr += 2;
              break;
            }
            *ptr = '\n';
          }
          ptr++;
        }
        if (strlen(line) == 0) break;

        if (!strncmp(line, "Application-Name:", 17)) {
          line += 18;
          while(isspace(*line)) line++;
          if (application_name) g_free(application_name);
          application_name = g_strdup(line);
        }
        if (!strncmp(line, "Application-Icon:", 17)) {
          line += 18;
          while(isspace(*line)) line++;
          if (application_icon) g_free(application_icon);
          application_icon = g_strdup(line);
        }
        if (!strncmp(line, "Notifications-Count:", 20)) {
          line += 21;
          while(isspace(*line)) line++;
          notifications_count = atol(line);
        }
      }
      int n;
      for (n = 0; n < notifications_count; n++) {
        char* notification_name = NULL;
        char* notification_icon = NULL;
        gboolean notification_enabled = FALSE;
        char* notification_display_name = NULL;
        while (*ptr) {
          char* line = ptr;
          while (*ptr) {
            if (*ptr == '\r') {
              if (*(ptr+1) == '\n') {
                *ptr = 0;
                ptr += 2;
                break;
              }
              *ptr = '\n';
            }
            ptr++;
          }
          if (strlen(line) == 0) break;

          if (!strncmp(line, "Notification-Name:", 18)) {
            line += 19;
            while(isspace(*line)) line++;
            if (notification_name) g_free(notification_name);
            notification_name = g_strdup(line);
          }
          if (!strncmp(line, "Notification-Icon:", 18)) {
            line += 19;
            while(isspace(*line)) line++;
            if (notification_icon) g_free(notification_icon);
            notification_icon = g_strdup(line);
          }
          if (!strncmp(line, "Notification-Enabled:", 21)) {
            line += 22;
            while(isspace(*line)) line++;
            notification_enabled = strcasecmp(line, "true") == 0;
          }
          if (!strncmp(line, "Notification-Display-Name:", 26)) {
            line += 27;
            while(isspace(*line)) line++;
            if (notification_display_name) g_free(notification_display_name);
            notification_display_name = g_strdup(line);
          }
        }

		const char* sql;
        sql = sqlite3_mprintf(
              "delete from notification where app_name = '%q' and name = '%q'",
                application_name, notification_name);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
		sqlite3_free((void*) sql);
        sql = sqlite3_mprintf(
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
        sqlite3_exec(db, sql, NULL, NULL, NULL);
		sqlite3_free((void*) sql);

        if (notification_name) g_free(notification_name);
        if (notification_icon) g_free(notification_icon);
        if (notification_display_name) g_free(notification_display_name);
      }
      if (n == notifications_count) {
        ptr = "GNTP/1.0 OK\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);
      } else {
        ptr = "GNTP/1.0 -ERROR Invalid data\r\n"
            "Error-Description: Invalid data\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);
      }
      if (application_name) g_free(application_name);
      if (application_icon) g_free(application_icon);
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
        char* line = ptr;
        while (*ptr) {
          if (*ptr == '\r') {
            if (*(ptr+1) == '\n') {
              *ptr = 0;
              ptr += 2;
              break;
            }
            *ptr = '\n';
          }
          ptr++;
        }
        if (strlen(line) == 0) break;

        if (!strncmp(line, "Application-Name:", 17)) {
          line += 18;
          while(isspace(*line)) line++;
          if (application_name) g_free(application_name);
          application_name = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Name:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          if (notification_name) g_free(notification_name);
          notification_name = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Title:", 19)) {
          line += 20;
          while(isspace(*line)) line++;
          if (ni->title) g_free(ni->title);
          ni->title = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Text:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          if (ni->text) g_free(ni->text);
          ni->text = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Icon:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          if (ni->icon) g_free(ni->icon);
          ni->icon = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Callback-Target:", 29)) {
          line += 30;
          while(isspace(*line)) line++;
          if (ni->url) g_free(ni->url);
          ni->url = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Display-Name:", 26)) {
          line += 27;
          while(isspace(*line)) line++;
          notification_display_name = g_strdup(line);
        }
      }

      if (ni->title && ni->text) {
        ptr = "GNTP/1.0 OK\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);

        gboolean enable = FALSE;
        const char* sql = sqlite3_mprintf(
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
		sqlite3_free((void*) sql);

        if (enable) {
          DISPLAY_PLUGIN* cp = current_display;
          if (notification_display_name) {
            int i, len = g_list_length(display_plugins);
            for (i = 0; i < len; i++) {
              DISPLAY_PLUGIN* dp =
                (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
              if (!g_strcasecmp(dp->name(), notification_display_name)) {
                cp = dp;
                break;
              }
            }
          }
          g_idle_add((GSourceFunc) cp->show, ni); // call once
        }
      } else {
        ptr = "GNTP/1.0 -ERROR Invalid data\r\n"
            "Error-Description: Invalid data\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);

        if (ni->title) g_free(ni->title);
        if (ni->text) g_free(ni->text);
        if (ni->icon) g_free(ni->icon);
        if (ni->url) g_free(ni->url);
        g_free(ni);
      }
      if (notification_name) g_free(notification_name);
      if (notification_display_name) g_free(notification_display_name);
      if (application_name) g_free(application_name);
    }
    free(data);
  } else {
    ptr = "GNTP/1.0 -ERROR Invalid command\r\n"
        "Error-Description: Invalid command\r\n\r\n";
    send(sock, ptr, strlen(ptr), 0);
  }
  free(top);
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  return NULL;

leave:
  ptr = "GNTP/1.0 -ERROR Invalid request\r\n"
      "Error-Description: Invalid request\r\n\r\n";
  send(sock, ptr, strlen(ptr), 0);
  free(top);
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  return NULL;
}

#ifdef _WIN32
static BOOL WINAPI
ctrl_handler(DWORD type) {
  gtk_main_quit();
  return TRUE;
}
#else
static void
signal_handler(int num) {
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
  GtkWidget* menu;
  GtkWidget* menu_item;

  // TODO: absolute path
  gchar* path = g_build_filename(DATADIR, "data", NULL);
  gchar* fullpath = g_build_filename(path, "icon.png", NULL);
  g_free(path);
  status_icon = gtk_status_icon_new_from_file(fullpath);
  g_free(fullpath);
  gtk_status_icon_set_tooltip(status_icon, "Growl");
  gtk_status_icon_set_visible(status_icon, TRUE);
  menu = gtk_menu_new();
  g_signal_connect(GTK_STATUS_ICON(status_icon), "popup-menu",
      G_CALLBACK(status_icon_popup), menu);

  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
  g_signal_connect(G_OBJECT(menu_item), "activate",
      G_CALLBACK(settings_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
  g_signal_connect(G_OBJECT(menu_item), "activate",
      G_CALLBACK(about_click), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
  g_signal_connect(G_OBJECT(menu_item), "activate",
      G_CALLBACK(exit_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  gtk_widget_show_all(menu);
}

static void
destroy_menu() {
  if (status_icon) {
      gtk_status_icon_set_visible(GTK_STATUS_ICON(status_icon), FALSE);
	  g_object_unref(G_OBJECT(status_icon));
  }
}

static gboolean
load_config() {
  gchar* confdir = (gchar*) g_get_user_config_dir();
  gchar* appdir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", NULL);
  g_free(confdir);
  if (g_mkdir_with_parents(appdir, 0700) < 0) {
    perror("mkdir");
    g_critical("Can't create directory: %s", appdir);
    g_free(appdir);
    return FALSE;
  }
  gchar* confdb = g_build_filename(appdir, "config.db", NULL);
  g_free(appdir);
  gboolean exist = g_file_test(confdb, G_FILE_TEST_EXISTS);
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

  gchar* version = get_config_string("version", "");
  if (strcmp(version, PACKAGE_VERSION)) {
    char* sqls[] = {
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
    char** sql = sqls;
    while (*sql) {
      sqlite3_exec(db, *sql, NULL, NULL, NULL);
      sql++;
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
  if (password) g_free(password);
  if (db) sqlite3_close(db);
}

static gboolean
load_display_plugins() {
  GDir *dir;
  const gchar *filename;
  gchar* path = g_build_filename(LIBDIR, "display", NULL);
  dir = g_dir_open(path, 0, NULL);
  if (!dir) {
    perror("open");
    g_critical("Display plugin directory isn't found: %s", path);
    return FALSE;
  }

  gchar* default_display = get_config_string("default_display", "Default");

  current_display = NULL;
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* fullpath = g_build_filename(path, filename, NULL);
    GModule* handle = g_module_open(fullpath, G_MODULE_BIND_LAZY);
    g_free(fullpath);
    if (!handle) {
      continue;
    }
    DISPLAY_PLUGIN* dp = g_new0(DISPLAY_PLUGIN, 1);
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
  int i, len = g_list_length(display_plugins);
  for (i = 0; i < len; i++) {
    DISPLAY_PLUGIN* dp = (DISPLAY_PLUGIN*) g_list_nth_data(display_plugins, i);
    if (dp->term) dp->term();
    g_module_close(dp->handle);
    g_free(dp);
  }
  g_list_free(display_plugins);
  display_plugins = NULL;
}

static void
subscribe_show(NOTIFICATION_INFO* ni) {
  g_idle_add((GSourceFunc) current_display->show, ni);
}

static gboolean
load_subscribe_plugins() {
  GDir *dir;
  const gchar *filename;
  gchar* path = g_build_filename(LIBDIR, "subscribe", NULL);
  dir = g_dir_open(path, 0, NULL);
  if (!dir) {
    g_warning("Subscribe plugin directory isn't found: %s", path);
    return TRUE;
  }

  sc.show = subscribe_show;
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* fullpath = g_build_filename(path, filename, NULL);
    GModule* handle = g_module_open(fullpath, G_MODULE_BIND_LAZY);
    g_free(fullpath);
    if (!handle) {
      continue;
    }
    SUBSCRIBE_PLUGIN* sp = g_new0(SUBSCRIBE_PLUGIN, 1);
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
  int i, len = g_list_length(subscribe_plugins);
  for (i = 0; i < len; i++) {
    SUBSCRIBE_PLUGIN* sp =
      (SUBSCRIBE_PLUGIN*) g_list_nth_data(subscribe_plugins, i);
    if (sp->term) sp->term();
    g_module_close(sp->handle);
    g_free(sp);
  }
  g_list_free(subscribe_plugins);
  subscribe_plugins = NULL;
}

static gboolean
gntp_accepted(GIOChannel* source, GIOCondition condition, gpointer data) {
  int fd = g_io_channel_unix_get_fd(source);
  int sock;
  struct sockaddr_in client;
  int client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if ((sock = accept(fd, (struct sockaddr *) &client,
          (socklen_t *) &client_len)) < 0) {
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
udp_recv_proc(GIOChannel* source, GIOCondition condition, gpointer data) {
  int is_local_app = FALSE;
  int fd = g_io_channel_unix_get_fd(source);
  char buf[BUFSIZ];
  memset(buf, 0, sizeof(buf));

  ssize_t len = recvfrom(fd, (char*) buf, sizeof(buf), 0, NULL, NULL);
  if (len > 0) {
    if (buf[0] == 1) {
      if (buf[1] == 0 || buf[1] == 2 || buf[1] == 4) {
        //GROWL_REGIST_PACKET* packet = (GROWL_REGIST_PACKET*) &buf[0];
      } else
      if (buf[1] == 1 || buf[1] == 3 || buf[1] == 5) {
        GROWL_NOTIFY_PACKET* packet = (GROWL_NOTIFY_PACKET*) &buf[0];
        if (packet->type == 1) {
          char digest[MD5_DIGEST_LENGTH] = {0};
          MD5_CTX ctx;
          MD5_Init(&ctx);
          MD5_Update(&ctx, buf, len - sizeof(digest));
          MD5_Update(&ctx, (char*) password, strlen(password));
          MD5_Final((unsigned char*) digest, &ctx);
          int n;
          for (n = 0; n < sizeof(digest); n++) {
            if (digest[n] != buf[len-sizeof(digest)+n])
              return TRUE;
          }
        } else
        if (packet->type == 3) {
          char digest[SHA256_DIGEST_LENGTH] = {0};
          SHA256_CTX ctx;
          SHA256_Init(&ctx);
          SHA256_Update(&ctx, buf, len - sizeof(digest));
          SHA256_Update(&ctx, password, strlen(password));
          SHA256_Final((unsigned char*) digest, &ctx);
          int n;
          for (n = 0; n < sizeof(digest); n++) {
            if (digest[n] != buf[len-sizeof(digest)+n])
              return TRUE;
          }
        } else {
          if (is_local_app && require_password_for_local_apps) goto leave;
          if (!is_local_app && require_password_for_lan_apps) goto leave;
        }
        NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
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

  struct sockaddr_in server_addr;
  memset((char *) &server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(9887);

  if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    return NULL;
  }

  fd_set fdset;
  FD_SET(fd, &fdset);
  GIOChannel* channel = g_io_channel_unix_new(fd);
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

  sockopt_t sockopt;
  sockopt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
        &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    return NULL;
  }
  sockopt = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
        &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    return NULL;
  }

  struct sockaddr_in server_addr;
  memset((char *) &server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(23053);

  if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
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
  GIOChannel* channel = g_io_channel_unix_new(fd);
  g_io_add_watch(channel, G_IO_IN | G_IO_ERR, gntp_accepted, NULL);
  g_io_channel_unref(channel);

  return channel;
}



static void
destroy_gntp_server(GIOChannel* channel) {
  if (channel) {
    closesocket(g_io_channel_unix_get_fd(channel));
    g_io_channel_unref(channel);
  }
}

static void
destroy_udp_server(GIOChannel* channel) {
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
  GIOChannel* gntp_fd;
  GIOChannel* udp_fd;

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
  if ((gntp_fd = create_gntp_server()) < 0) goto leave;
  if ((udp_fd = create_udp_server()) < 0) goto leave;
  if (!load_display_plugins()) goto leave;
  if (!load_subscribe_plugins()) goto leave;
  create_menu();

  gtk_main();

leave:
  destroy_menu();
  unload_subscribe_plugins();
  unload_display_plugins();
  destroy_gntp_server(gntp_fd);
  destroy_udp_server(udp_fd);
  unload_config();
  if (exepath) g_free(exepath);

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

// vim:set et sw=2 ts=2 ai:
