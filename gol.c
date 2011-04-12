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
# include <netinet/tcp.h>
# include <sys/socket.h>
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
#endif

static gchar* password = NULL;
static gboolean main_loop = TRUE;
typedef gboolean (*notification_init_fn)(gchar* datadir);
static notification_init_fn notification_init = NULL;
typedef gboolean (*notification_show_fn)(NOTIFICATION_INFO* ni);
static notification_show_fn notification_show = NULL;
typedef gboolean (*notification_term_fn)();
static notification_term_fn notification_term = NULL;

static long
readall(int fd, char** ptr) {
  int i = 0, r;
  *ptr = (char*) calloc(BUFSIZ, 1);
  while (*ptr && (r = recv(fd, *ptr + i, BUFSIZ, 0)) > 0) {
    i += r;
    if (r > 2 && !strncmp(*ptr + i - 4, "\r\n\r\n", 4)) break;
    *ptr = realloc(*ptr, BUFSIZ + i);
  }
  return i;
}

unsigned int
unhex(unsigned char c) {
  if('0' <= c && c <= '9') return (c - '0');
  if('a' <= c && c <= 'f') return (0x0a + c - 'a');
  if('A' <= c && c <= 'F') return (0x0a + c - 'A');
  return 0;
}

static void
status_icon_popup(GtkStatusIcon* status_icon, guint button, guint32 activate_time, gpointer menu) {
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static void
settings_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  GtkWidget* dialog;
  GtkWidget* toolbar;
  GtkToolItem* toolitem;
  GtkWidget* vbox;

  dialog = gtk_dialog_new_with_buttons(
      "Settings", NULL, GTK_DIALOG_MODAL,
      GTK_STOCK_OK, GTK_RESPONSE_OK,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);
  gtk_window_set_icon_from_file(GTK_WINDOW(dialog), "./data/icon.png", NULL);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

  toolbar = gtk_toolbar_new();
  gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_PREFERENCES);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_EXECUTE);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_SELECT_COLOR);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_NETWORK);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_DIALOG_AUTHENTICATION);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  toolitem = gtk_tool_button_new_from_stock(GTK_STOCK_INDEX);
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  gint w = 48, h = 48;
  gtk_icon_size_lookup(
      gtk_toolbar_get_icon_size(GTK_TOOLBAR(toolbar)), &w, &h);
  toolitem = gtk_tool_button_new(gtk_image_new_from_pixbuf(
    gdk_pixbuf_scale_simple(
      gdk_pixbuf_new_from_file("./data/icon.png", NULL), w, h, GDK_INTERP_TILES)), "About");
  gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolitem, -1);
  gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), toolbar, FALSE, FALSE, 0);

  vbox = gtk_vbox_new(TRUE, 5);
  gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), vbox, TRUE, TRUE, 0);

  gtk_widget_set_size_request(dialog, 300, 200);

  gtk_widget_show_all(dialog);
}

static void
about_click(GtkWidget* widget, gpointer user_data) {
  const gchar* authors[2] = {"mattn", NULL};
  gchar* contents = NULL;
  gchar* utf8 = NULL;
  GdkPixbuf* logo = NULL;
  GtkWidget* dialog;
  dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), "Growl For Linux");
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
  if (g_file_get_contents("COPYING", &contents, NULL, NULL)) {
    utf8 = g_locale_to_utf8(contents, -1, NULL, NULL, NULL);
    g_free(contents);
    contents = NULL;
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog), utf8);
    g_free(utf8);
    utf8 = NULL;
  }
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://mattn.kaoriya.net/");
  logo = gdk_pixbuf_new_from_file("./data/growl4linux.jpg", NULL);
  gtk_about_dialog_set_logo (GTK_ABOUT_DIALOG(dialog), logo);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void
exit_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  main_loop = FALSE;
}

static gpointer
recv_thread(gpointer data) {
  int sock = (int) data;
  int need_to_show = 0;

  NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
  if (!ni) {
    perror("g_new0");
  }

  char* ptr;
  int r = readall(sock, &ptr);
  char* top = ptr;
  char* end = ptr + r;
  if (!strncmp(ptr, "GNTP/1.0 ", 9)) {
    ptr += 9;
    if (!strncmp(ptr, "REGISTER ", 9)) {
      ptr += 9;
      // TODO: register
    } else
    if (!strncmp(ptr, "NOTIFY ", 7)) {
      ptr += 7;
      char* data = NULL;
      if (!strncmp(ptr, "NONE", 4) && strchr("\n ", *(ptr+5))) {
        ptr = strchr(ptr, '\r');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;
        data = (char*) calloc(r-(ptr-top)-4+1, 1);
        if (!data) goto leave;
        memcpy(data, ptr, r-(ptr-top)-4);
      } else {
        if (strncmp(ptr, "AES:", 4) &&
            strncmp(ptr, "DES:", 4) &&
            strncmp(ptr, "3DES:", 5)) goto leave;

        char* crypt_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* iv;
        iv = ptr;
        ptr = strchr(ptr, ' ');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;

        if (strncmp(ptr, "MD5:", 4) &&
            strncmp(ptr, "SHA1:", 5) &&
            strncmp(ptr, "SHA256:", 7)) goto leave;

        char* hash_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* key = ptr;
        ptr = strchr(ptr, '.');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* salt = ptr;

        ptr = strchr(ptr, '\r');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;

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
      while (*ptr) {
        char* line = ptr;
        ptr = strchr(ptr, '\r');
        if (!ptr) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;
        if (!strncmp(line, "Notification-Title:", 19)) {
          line += 20;
          while(isspace(*line)) line++;
          ni->title = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Text:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->text = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Icon:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->icon = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Callback-Target:", 29)) {
          line += 30;
          while(isspace(*line)) line++;
          ni->url = g_strdup(line);
        }
      }

      if (ni->title && ni->text)
        need_to_show = 1;
      else {
        ptr = "GNTP/1.0 -ERROR Invalid data\r\n"
            "Error-Description: Invalid data\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);
      }
      free(data);
    }
    ptr = "GNTP/1.0 OK\r\n\r\n";
    send(sock, ptr, strlen(ptr), 0);
  } else {
    ptr = "GNTP/1.0 -ERROR Invalid command\r\n\r\n";
    send(sock, ptr, strlen(ptr), 0);
  }
  free(top);
  closesocket(sock);
  if (need_to_show)
    g_idle_add((GSourceFunc) notification_show, ni); // call once
  else {
    g_free(ni->title);
    g_free(ni->text);
    g_free(ni->icon);
    g_free(ni->url);
    g_free(ni);
  }
  return NULL;

leave:
  free(top);
  closesocket(sock);
  free(ni);
  return NULL;
}

static void
signal_handler(int num) {
  main_loop = FALSE;
}

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

int
main(int argc, char* argv[]) {
  int fd;
  struct sockaddr_in server_addr;
  fd_set fdset;
  struct timeval tv;
  GtkStatusIcon* status_icon;
  GtkWidget* menu;
  GtkWidget* menu_item;
  sockopt_t sockopt;

  tv.tv_sec = 0;
  tv.tv_usec = 0;

#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

#ifdef G_THREADS_ENABLED
  g_thread_init(NULL);
#endif

  gtk_init(&argc, &argv);
  // TODO: absolute path
  status_icon = gtk_status_icon_new_from_file("./data/icon.png");
  gtk_status_icon_set_tooltip(status_icon, "Growl");
  gtk_status_icon_set_visible(status_icon, TRUE);
  menu = gtk_menu_new();
  g_signal_connect(GTK_STATUS_ICON(status_icon), "popup-menu", G_CALLBACK(status_icon_popup), menu);

  menu_item = gtk_menu_item_new_with_label("Settings");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(settings_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
  menu_item = gtk_menu_item_new_with_label("About");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(about_click), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  menu_item = gtk_menu_item_new_with_label("Exit");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(exit_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
  gtk_widget_show_all(menu);

  sqlite3 *db;
  char* error;
  gchar* confdir = (gchar*) g_get_user_config_dir();
  confdir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", NULL);
  g_mkdir_with_parents(confdir, 0700);
  gchar* confdb = g_build_filename(confdir, "config.db", NULL);
  int rc;
  if (!g_file_test(confdb, G_FILE_TEST_EXISTS)) {
    char* sqls[] = {
      "create table config(key text not null primary key, value text not null)",
      "create table notification(name text not null primary key, enable bool not null, display text not null, sticky bool not null)",
      NULL
    };
    char** sql = sqls;
    rc = sqlite3_open(confdb, &db);
    while (*sql) {
      rc = sqlite3_exec(db, *sql, 0, 0, &error);
      sql++;
    }
    sqlite3_close(db);
  }
  rc = sqlite3_open(confdb, &db);

  const char* sql = "select value from config where key = 'password'";
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    password = g_strdup((char*) sqlite3_column_text(stmt, 0));
  } else {
    password = g_strdup("");
  }

  //gchar* path = g_module_build_path("./display/default", "default");
  gchar* path = g_module_build_path("./display/balloon", "balloon");
  GModule* handle = g_module_open(path, G_MODULE_BIND_LAZY);
  g_free(path);
  if (!g_module_symbol(handle, "notification_show", (void**) &notification_show)) {
    g_module_close(handle);
    perror("g_module_open");
    exit(1);
  }
  g_module_symbol(handle, "notification_init", (void**) &notification_init);
  g_module_symbol(handle, "notification_term", (void**) &notification_term);
  g_module_make_resident(handle);

  if (notification_init) {
    notification_init("./display/balloon");
  }

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    exit(1);
  }

  sockopt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }
  sockopt = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }

  memset((char *) &server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(23053);

  if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    exit(1);
  }

  if (listen(fd, 5) < 0) {
    perror("listen");
    closesocket(fd);
    exit(1);
  }

  while (main_loop) {
    gtk_main_iteration_do(FALSE);
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    if (select(FD_SETSIZE, &fdset, NULL, NULL, &tv) < 0) {
      perror("select");
      continue;
    }
    if (!FD_ISSET(fd, &fdset))
      continue;
    struct sockaddr_in client;
    int sock;
    int client_len = sizeof(client);
    memset(&client, 0, sizeof(client));
    if ((sock = accept(fd, (struct sockaddr *) &client, (socklen_t *) &client_len)) < 0) {
      perror("accept");
      continue;
    }
    sockopt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));

#ifdef G_THREADS_ENABLED
    g_thread_create(recv_thread, (gpointer) sock, FALSE, NULL);
#else
    recv_thread((gpointer) sock);
#endif
  }

  sqlite3_close(db);

  gtk_status_icon_set_visible(GTK_STATUS_ICON(status_icon), FALSE);

#ifdef _WIN32
  WSACleanup();
#endif

  return 0;
}

// vim:set et sw=2 ts=2 ai:
