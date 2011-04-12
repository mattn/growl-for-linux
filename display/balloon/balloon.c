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
#include <curl/curl.h>
#include "../../gol.h"
#include "mask.xpm"

#define REQUEST_TIMEOUT            (5)

#ifdef _WIN32
# ifndef strncasecmp
#  define strncasecmp(d,s,n) strnicmp(d,s,n)
# endif
#endif

static GList* notifications = NULL;
static gchar* datadir = NULL;
static GdkPixmap* pixmap = NULL;
static GdkBitmap* bitmap = NULL;
static gint pixmap_width, pixmap_height;

typedef struct {
  NOTIFICATION_INFO* ni;
  gint pos;
  gint x, y;
  gint timeout;
  GtkWidget* popup;
  gint offset;
} DISPLAY_INFO;

typedef struct {
  char* data;     // response data from server
  size_t size;    // response size of data
} MEMFILE;

static MEMFILE*
memfopen() {
  MEMFILE* mf = (MEMFILE*) malloc(sizeof(MEMFILE));
  if (mf) {
    mf->data = NULL;
    mf->size = 0;
  }
  return mf;
}

static void
memfclose(MEMFILE* mf) {
  if (mf->data) free(mf->data);
  free(mf);
}

static size_t
memfwrite(char* ptr, size_t size, size_t nmemb, void* stream) {
  MEMFILE* mf = (MEMFILE*) stream;
  int block = size * nmemb;
  if (!mf) return block; // through
  if (!mf->data)
    mf->data = (char*) malloc(block);
  else
    mf->data = (char*) realloc(mf->data, mf->size + block);
  if (mf->data) {
    memcpy(mf->data + mf->size, ptr, block);
    mf->size += block;
  }
  return block;
}

static char*
memfstrdup(MEMFILE* mf) {
  char* buf;
  if (mf->size == 0) return NULL;
  buf = (char*) malloc(mf->size + 1);
  memcpy(buf, mf->data, mf->size);
  buf[mf->size] = 0;
  return buf;
}

static char*
get_http_header_alloc(const char* ptr, const char* key) {
  const char* tmp = ptr;

  while (*ptr) {
    tmp = strpbrk(ptr, "\r\n");
    if (!tmp) break;
    if (!strncasecmp(ptr, key, strlen(key)) && *(ptr + strlen(key)) == ':') {
      size_t len;
      char* val;
      const char* top = ptr + strlen(key) + 1;
      while (*top && isspace(*top)) top++;
      if (!*top) return NULL;
      len = tmp - top + 1;
      val = malloc(len);
      memset(val, 0, len);
      strncpy(val, top, len-1);
      return val;
    }
    ptr = tmp + 1;
  }
  return NULL;
}

static GdkPixbuf*
url2pixbuf(const char* url, GError** error) {
  GdkPixbuf* pixbuf = NULL;
  GdkPixbufLoader* loader = NULL;
  GError* _error = NULL;

  if (!strncmp(url, "file:///", 8) || g_file_test(url, G_FILE_TEST_EXISTS)) {
    gchar* newurl = g_filename_from_uri(url, NULL, NULL);
    pixbuf = gdk_pixbuf_new_from_file(newurl ? newurl : url, &_error);
  } else {
    CURL* curl = NULL;
    MEMFILE* mbody;
    MEMFILE* mhead;
    char* head;
    char* body;
    unsigned long size;
    CURLcode res = CURLE_FAILED_INIT;

    curl = curl_easy_init();
    if (!curl) return NULL;

    mbody = memfopen();
    mhead = memfopen();

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mbody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, mhead);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    head = memfstrdup(mhead);
    memfclose(mhead);
    body = memfstrdup(mbody);
    size = mbody->size;
    memfclose(mbody);

    if (res == CURLE_OK) {
      char* ctype;
      char* csize;
      ctype = get_http_header_alloc(head, "Content-Type");
      csize = get_http_header_alloc(head, "Content-Length");

#ifdef _WIN32
      if (ctype &&
          (!strcmp(ctype, "image/jpeg") || !strcmp(ctype, "image/gif"))) {
        char temp_path[MAX_PATH];
        char temp_filename[MAX_PATH];
        FILE* fp;
        GetTempPath(sizeof(temp_path), temp_path);
        GetTempFileName(temp_path, "growl-for-linux-", 0, temp_filename);
        fp = fopen(temp_filename, "wb");
        if (fp) {
          fwrite(body, size, 1, fp);
          fclose(fp);
        }
        pixbuf = gdk_pixbuf_new_from_file(temp_filename, NULL);
        DeleteFile(temp_filename);
      } else
#endif
      {
        if (ctype)
          loader =
            (GdkPixbufLoader*) gdk_pixbuf_loader_new_with_mime_type(ctype,
                error);
        if (csize)
          size = atol(csize);
        if (!loader) loader = gdk_pixbuf_loader_new();
        if (body && gdk_pixbuf_loader_write(loader, (const guchar*) body,
              size, &_error)) {
          pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        }
      }
      if (ctype) free(ctype);
      if (csize) free(csize);
      if (loader) gdk_pixbuf_loader_close(loader, NULL);
    } else {
      _error = g_error_new_literal(G_FILE_ERROR, res,
          curl_easy_strerror(res));
    }

    free(head);
    free(body);
  }

  /* cleanup callback data */
  if (error && _error) *error = _error;
  return pixbuf;
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
notification_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  DISPLAY_INFO* di = (DISPLAY_INFO*) user_data;
  if (di->timeout >= 30) di->timeout = 30;
  if (di->ni->url && *di->ni->url) open_url(di->ni->url);
}

static gboolean
notification_animation_func(gpointer data) {
  DISPLAY_INFO* di = (DISPLAY_INFO*) data;

  if (di->timeout-- < 0) {
    gtk_widget_destroy(di->popup);
    notifications = g_list_remove(notifications, di);
    g_free(di->ni->title);
    g_free(di->ni->text);
    g_free(di->ni->icon);
    g_free(di->ni->url);
    g_free(di->ni);
    g_free(di);
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

static gint
notifications_compare(gconstpointer a, gconstpointer b) {
  return ((DISPLAY_INFO*)b)->pos < ((DISPLAY_INFO*)a)->pos;
}

static gboolean
notification_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
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

G_MODULE_EXPORT gboolean
notification_show(NOTIFICATION_INFO* ni) {
  GdkColor color;
  GtkWidget* fixed;
  GtkWidget* vbox;
  GtkWidget* hbox;
  GtkWidget* label;
  GtkWidget* image;
  GdkPixbuf* pixbuf;
  GdkScreen* screen;
  gint n, pos, len;
  gint x, y;
  gint monitor_num;
  GdkRectangle rect;

  DISPLAY_INFO* di = g_new0(DISPLAY_INFO, 1);
  if (!di) {
    perror("g_new0");
  }
  di->ni = ni;

  len = g_list_length(notifications);
  for (pos = 0; pos < len; pos++) {
    DISPLAY_INFO* p = g_list_nth_data(notifications, pos);
    if (pos != p->pos) break;
  }

  screen = gdk_screen_get_default();
  monitor_num = gdk_screen_get_primary_monitor(screen);
  gdk_screen_get_monitor_geometry(screen, monitor_num, &rect);

  x = rect.x + rect.width - 250;
  y = rect.y + rect.height - 110;
  for (n = 0; n < pos; n++) {
    y -= 110;
    if (y < 0) {
      x -= 250;
      if (x < 0) {
        return FALSE;
      }
      y = rect.y + rect.height - 110;
    }
  }

  di->pos = pos;
  notifications = g_list_insert_sorted(notifications, di, notifications_compare);
  di->x = x;
  di->y = y;

  di->popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_title(GTK_WINDOW(di->popup), "growl-for-linux");
  gtk_window_set_resizable(GTK_WINDOW(di->popup), TRUE);
  gtk_window_set_decorated(GTK_WINDOW(di->popup), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(di->popup), TRUE);

  gtk_window_stick(GTK_WINDOW(di->popup));

  fixed = gtk_fixed_new();
  gtk_container_set_border_width(GTK_CONTAINER(fixed), 30);
  gtk_container_add(GTK_CONTAINER(di->popup), fixed);

  vbox = gtk_vbox_new(FALSE, 5);
  gtk_container_add(GTK_CONTAINER(fixed), vbox);

  hbox = gtk_hbox_new(FALSE, 5);

  if (di->ni->icon && *di->ni->icon) {
    pixbuf = url2pixbuf(di->ni->icon, NULL);
    if (pixbuf) {
      GdkPixbuf* tmp = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
      if (tmp) pixbuf = tmp;
      image = gtk_image_new_from_pixbuf(pixbuf);
      gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
    }
  }

  PangoFontDescription* font_desc = pango_font_description_new();
  pango_font_description_set_family(font_desc, "Arial");
  pango_font_description_set_size(font_desc, 20);

  label = gtk_label_new(di->ni->title);
  gdk_color_parse("white", &color);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, &color);
  gtk_widget_modify_font(label, font_desc);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  label = gtk_label_new(di->ni->text);
  gdk_color_parse("white", &color);
  gtk_widget_modify_fg(label, GTK_STATE_NORMAL, &color);
//  gtk_widget_modify_font(label, font_desc);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, FALSE, 0);

//  pango_font_description_free(font_desc);

  gtk_widget_set_events(di->popup, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(di->popup), "button-press-event", G_CALLBACK(notification_clicked), di);

  di->offset = 0;
  di->timeout = 500;

  gtk_window_move(GTK_WINDOW(di->popup), di->x, di->y);
  gtk_window_set_opacity(GTK_WINDOW(di->popup), 0);
  gtk_widget_set_app_paintable(di->popup, TRUE);
  gtk_widget_set_double_buffered(di->popup, FALSE);
  gtk_widget_show_all(di->popup);

  if (pixmap == NULL) {
     pixmap = gdk_pixmap_create_from_xpm_d(di->popup->window, &bitmap, NULL, mask_xpm);
  }
  gdk_drawable_get_size(pixmap, &pixmap_width, &pixmap_height);
  gtk_widget_set_size_request(fixed, pixmap_width, pixmap_height);
  gdk_window_shape_combine_mask(di->popup->window, bitmap, 0, 0);
  g_signal_connect(G_OBJECT(di->popup), "expose-event", G_CALLBACK(notification_expose), di);

  g_timeout_add(10, notification_animation_func, di);

  return FALSE;
}

G_MODULE_EXPORT gboolean
notification_init(gchar* _datadir) {
  datadir = g_strdup(_datadir);
  return TRUE;
}

G_MODULE_EXPORT void
notification_term() {
}

// vim:set et sw=2 ts=2 ai:
