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
#include <gmodule.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <ctype.h>
#include <stdlib.h>
#include <memory.h>
#include <curl/curl.h>
#include "../../gol.h"

#define REQUEST_TIMEOUT            (5)

SUBSCRIPTOR_CONTEXT* sc = NULL;
gchar* last_id = NULL;

#define XML_CONTENT(x) (x->children ? (char*) x->children->content : NULL)

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

static gboolean
delay_show(gpointer data) {
  sc->show((NOTIFICATION_INFO*) data);
  return FALSE;
}

static gboolean
fetch_feed(gpointer data) {
  CURL* curl = NULL;
  CURLcode res = CURLE_OK;
  long http_status = 0;

  MEMFILE* mbody = NULL;
  char* body = NULL;

  xmlDocPtr doc = NULL;
  xmlNodeSetPtr nodes = NULL;
  xmlXPathContextPtr ctx = NULL;
  xmlXPathObjectPtr path = NULL;

  mbody = memfopen();
  curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(curl, CURLOPT_URL, "https://api.twitter.com/1/statuses/public_timeline.xml");
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

  if (res != CURLE_OK) {
    goto leave;
  }
  if (http_status == 304) {
    goto leave;
  }
  if (http_status != 200) {
    goto leave;
  }

  doc = body ? xmlParseDoc((xmlChar*) body) : NULL;
  if (!doc) goto leave;
  ctx = xmlXPathNewContext(doc);
  if (!ctx) goto leave;
  path = xmlXPathEvalExpression((xmlChar*)"/statuses/status", ctx);
  if (!path || xmlXPathNodeSetIsEmpty(path->nodesetval)) goto leave;
  nodes = path->nodesetval;
  int n, length = xmlXPathNodeSetGetLength(nodes);
  gchar* first_id = NULL;
  for(n = 0; n < length; n++) {
    char* id = NULL;
    char* user_id = NULL;
    char* icon = NULL;
    char* real = NULL;
    char* user_name = NULL;
    char* text = NULL;
    char* date = NULL;

    xmlNodePtr status = nodes->nodeTab[n];
    if (status->type != XML_ATTRIBUTE_NODE && status->type != XML_ELEMENT_NODE && status->type != XML_CDATA_SECTION_NODE) continue;
    status = status->children;
    while(status) {
      if (!strcmp("id", (char*) status->name)) id = (char*) status->children->content;
      if (!strcmp("created_at", (char*) status->name)) date = (char*) status->children->content;
      if (!strcmp("text", (char*) status->name)) {
        if (status->children) text = (char*) status->children->content;
      }
      /* user nodes */
      if (!strcmp("user", (char*) status->name)) {
        xmlNodePtr user = status->children;
        while(user) {
          if (!strcmp("id", (char*) user->name)) user_id = XML_CONTENT(user);
          if (!strcmp("name", (char*) user->name)) real = XML_CONTENT(user);
          if (!strcmp("screen_name", (char*) user->name)) user_name = XML_CONTENT(user);
          if (!strcmp("profile_image_url", (char*) user->name)) {
            icon = XML_CONTENT(user);
            icon = (char*) g_strchomp((gchar*) icon);
            icon = (char*) g_strchug((gchar*) icon);
          }
          user = user->next;
        }
      }
      status = status->next;
    }
    if (!first_id) first_id = id;
    if (id && last_id && !strcmp(id, last_id)) break;

    if (text && user_id) {
      NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
      ni->title = g_strdup(user_name);
      ni->text = g_strdup(text);
      ni->icon = g_strdup(icon);
      g_timeout_add(1000 * (n+1), delay_show, ni);
    }
  }
  if (last_id) g_free(last_id);
  if (first_id) last_id = g_strdup(first_id);

leave:
  if (body) free(body);
  if (path) xmlXPathFreeObject(path);
  if (ctx) xmlXPathFreeContext(ctx);
  if (doc) xmlFreeDoc(doc);
   
  g_timeout_add(1000 * length, fetch_feed, NULL);
  return FALSE;
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
  g_timeout_add(10, fetch_feed, NULL);
  return TRUE;
}

G_MODULE_EXPORT void
subscribe_stop() {
}

G_MODULE_EXPORT gchar*
subscribe_name() {
  return "Feed";
}

G_MODULE_EXPORT gchar*
subscribe_description() {
  return "<span size=\"large\"><b>Feed</b></span>\n"
    "<span>This is feed subscriber.</span>\n"
    "<span>Polling feed, and show notification.</span>\n";
}

G_MODULE_EXPORT char**
subscribe_thumbnail() {
  return NULL;
}

// vim:set et sw=2 ts=2 ai:
