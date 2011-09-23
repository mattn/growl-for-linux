/* Copyright 2011 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
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
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <gmodule.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <curl/curl.h>

#include "gol.h"
#include "plugins/memfile.h"

#define REQUEST_TIMEOUT            (5)

static SUBSCRIPTOR_CONTEXT* sc;
static DBusGConnection *conn;
static DBusGProxy* proxy;
static gboolean enable = FALSE;

static gchar* last_title;
static gchar* last_artist;
static gchar* last_album;

#define XML_CONTENT(x) (x->children ? (char*) x->children->content : NULL)

static gboolean
delay_show(gpointer data) {
  if (!enable) return FALSE;
  sc->show((NOTIFICATION_INFO*) data);
  return FALSE;
}

static char*
urlencode_alloc(const char* url) {
  const size_t len = strlen(url);
  char* temp = (char*) malloc(len * 3 + 1);
  char* const ret = temp;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) url[i];
    if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.~-", c))
      *temp++ = c;
    else {
      char buf[3] = {0};
      snprintf(buf, sizeof(buf), "%02x", c);
      *temp++ = '%';
      *temp++ = toupper(buf[0]);
      *temp++ = toupper(buf[1]);
    }
  }
  *temp = 0;
  return ret;
}

static gchar*
get_album_art(const char* artist, const char* album) {
  CURL* curl = NULL;
  CURLcode res = CURLE_OK;
  long http_status = 0;

  MEMFILE* mbody = NULL;
  char* body = NULL;

  xmlDocPtr doc = NULL;
  xmlXPathContextPtr ctx = NULL;
  xmlXPathObjectPtr path = NULL;

  char* info = g_strdup_printf("%s %s", artist, album);
  char* qinfo = urlencode_alloc(info);
  g_free(info);
  gchar* url = g_strdup_printf(
    "http://api.search.yahoo.com/ImageSearchService/V1/imageSearch?"
    "appid=%s&query=%s&type=all&results=10&start=1&format=any&adult_ok=True",
    "YahooExample",
    qinfo);

  mbody = memfopen();
  curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memfwrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, mbody);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  res = curl_easy_perform(curl);
  if (res == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_status);
  curl_easy_cleanup(curl);

  body = memfstrdup(mbody);
  memfclose(mbody);

  gchar* image_url = NULL;
  if (res != CURLE_OK || http_status != 200) goto leave;

  // XXX: too deep!!!!!!!!!!!!!!!!!
  doc = body ? xmlParseDoc((xmlChar*) body) : NULL;
  xmlNodePtr node = doc->children;
  if (strcmp((const char*) node->name, "ResultSet")) goto leave;
  for (node = node->children; node; node = node->next) {
    if (strcmp((const char*) node->name, "Result")) continue;
    for (node = node->children; node; node = node->next) {
      if (strcmp((const char*) node->name, "Thumbnail")) continue;
      for (node = node->children; node; node = node->next) {
        if (strcmp((const char*) node->name, "Url")) continue;
        image_url = g_strdup(XML_CONTENT(node));
        break;
      }
      if (image_url) break;
    }
    if (image_url) break;
  }

leave:
  free(body);
  if (path) xmlXPathFreeObject(path);
  if (ctx) xmlXPathFreeContext(ctx);
  if (doc) xmlFreeDoc(doc);

  return image_url;
}

// FIXME: too long!!!!!!!!!!!!!!!!!!!!!
static gboolean
get_rhythmbox_info(gpointer GOL_UNUSED_ARG(data)) {
  if (!enable) return FALSE;

  DBusGProxy *player = NULL;
  DBusGProxy *shell = NULL;
  GError *error = NULL;

  if (!conn) {
    conn = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
    if (error) g_error_free(error);
    if (!conn) return FALSE;
  }

  if (!proxy) {
    proxy = dbus_g_proxy_new_for_name(
        conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus");
    if (error) g_error_free(error);
    if (!proxy) return FALSE;
  }

  gboolean exists = FALSE;
  if (dbus_g_proxy_call_with_timeout(
        proxy,
        "NameHasOwner",
        5000,
        &error,
        G_TYPE_STRING,
        "org.gnome.Rhythmbox",
        G_TYPE_INVALID,
        G_TYPE_BOOLEAN,
        &exists,
        G_TYPE_INVALID)) {
    if (error) g_error_free(error);
    if (!exists) return TRUE;
  }

  if (!shell) {
    shell = dbus_g_proxy_new_for_name(
        conn,
        "org.gnome.Rhythmbox",
        "/org/gnome/Rhythmbox/Shell",
        "org.gnome.Rhythmbox.Shell");
  }

  if (!player) {
    player = dbus_g_proxy_new_for_name(
        conn,
        "org.gnome.Rhythmbox",
        "/org/gnome/Rhythmbox/Player",
        "org.gnome.Rhythmbox.Player");
  }

  gboolean playing;
  if (!dbus_g_proxy_call_with_timeout(
        player,
        "getPlaying",
        5000,
        &error,
        G_TYPE_INVALID,
        G_TYPE_BOOLEAN,
        &playing,
        G_TYPE_INVALID)) {
    if (error) g_error_free(error);
    if (!playing) return TRUE;
  }

  char *uri;
  if (!dbus_g_proxy_call_with_timeout(
        player,
        "getPlayingUri",
        5000,
        &error,
        G_TYPE_INVALID,
        G_TYPE_STRING,
        &uri,
        G_TYPE_INVALID)) {
    if (error) g_error_free(error);
    return TRUE;
  }

  GHashTable *table;
  if (!dbus_g_proxy_call_with_timeout(
        shell,
        "getSongProperties",
        5000,
        &error,
        G_TYPE_STRING,
        uri,
        G_TYPE_INVALID,
        dbus_g_type_get_map("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
        &table,
        G_TYPE_INVALID)) {
    if (error) g_error_free(error);
    return TRUE;
  }
  g_free(uri);

  gchar* title = NULL;
  gchar* artist = NULL;
  gchar* album = NULL;

  GValue* value = (GValue*) g_hash_table_lookup(table, "rb:stream-song-title");
  if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
    title = g_strdup(g_value_get_string(value));
  } else {
    value = (GValue*) g_hash_table_lookup(table, "title");
    if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
      title = g_strdup(g_value_get_string(value));
    }
  }
  value = (GValue*) g_hash_table_lookup(table, "artist");
  if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
    artist = g_strdup(g_value_get_string(value));
  }
  value = (GValue*) g_hash_table_lookup(table, "album");
  if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
    album = g_strdup(g_value_get_string(value));
  }

  g_hash_table_destroy(table);

  if (title && artist && album &&
          (!last_title  || strcmp(last_title, title)) &&
          (!last_artist || strcmp(last_artist, artist)) &&
          (!last_album  || strcmp(last_album, album))) {
    NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
    ni->title = g_strdup(title);
    ni->text = g_strdup_printf("%s\n%s", album, artist);
    ni->icon = get_album_art(artist, album);
    g_timeout_add(10, delay_show, ni);

    g_free(last_title);
    g_free(last_artist);
    g_free(last_album);
    last_title = title;
    last_artist = artist;
    last_album = album;
  } else {
    g_free(title);
    g_free(artist);
    g_free(album);
  }

  return TRUE;
}

G_MODULE_EXPORT gboolean
subscribe_init(SUBSCRIPTOR_CONTEXT* _sc) {
  sc = _sc;
  return TRUE;
}

G_MODULE_EXPORT void
subscribe_term() {
}

G_MODULE_EXPORT gboolean
subscribe_start() {
  enable = TRUE;
  g_timeout_add(5000, get_rhythmbox_info, NULL);
  return TRUE;
}

G_MODULE_EXPORT void
subscribe_stop() {
  enable = FALSE;
}

G_MODULE_EXPORT const gchar*
subscribe_name() {
  return "Rhythmbox";
}

G_MODULE_EXPORT const gchar*
subscribe_description() {
  return
    "<span size=\"large\"><b>Rhythmbox</b></span>\n"
    "<span>This is rhythmbox subscriber.</span>\n"
    "<span>Show now playing music in rhythmbox.</span>\n";
}

G_MODULE_EXPORT char**
subscribe_thumbnail() {
  return NULL;
}

// vim:set et sw=2 ts=2 ai:
