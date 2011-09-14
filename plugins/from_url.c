#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include <curl/curl.h>

#include "memfile.h"
#include "from_url.h"

#define REQUEST_TIMEOUT (5)

CURLcode
memfile_from_url(const memfile_from_url_info info) {
  CURL* curl = curl_easy_init();
  if (!curl) return CURLE_FAILED_INIT;

  *info.body = memfopen();
  *info.header = memfopen();

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(curl, CURLOPT_URL, info.url);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, info.body_writer);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, *info.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, info.header_writer);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, *info.header);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  return res;
}

static char*
get_http_header_alloc(const char* ptr, const char* key) {
  while (*ptr) {
    const char* tmp = strpbrk(ptr, "\r\n");
    if (!tmp) break;
    if (!strncasecmp(ptr, key, strlen(key)) && *(ptr + strlen(key)) == ':') {
      const char* top = ptr + strlen(key) + 1;
      while (*top && isspace(*top)) top++;
      if (!*top) return NULL;
      const size_t len = tmp - top + 1;
      char* val = malloc(len);
      strncpy(val, top, len-1);
      val[len-1] = 0;
      return val;
    }
    ptr = tmp + 1;
  }
  return NULL;
}

GdkPixbuf*
pixbuf_from_url(const char* url, GError** error) {
  MEMFILE* mbody;
  MEMFILE* mhead;
  const CURLcode res = memfile_from_url((memfile_from_url_info){
    .url           = url,
    .body          = &mbody,
    .header        = &mhead,
    .body_writer   = memfwrite,
    .header_writer = memfwrite,
  });
  if (res == CURLE_FAILED_INIT) return NULL;

  char* head = memfstrdup(mhead);
  char* body = memfstrdup(mbody);
  unsigned long size = mbody->size;
  memfclose(mhead);
  memfclose(mbody);

  GdkPixbuf* pixbuf = NULL;
  GdkPixbufLoader* loader = NULL;
  GError* _error = NULL;

  if (res == CURLE_OK) {
    char* ctype = get_http_header_alloc(head, "Content-Type");
    char* csize = get_http_header_alloc(head, "Content-Length");

#ifdef _WIN32
    if (ctype &&
        (!strcmp(ctype, "image/jpeg") || !strcmp(ctype, "image/gif"))) {
      char temp_path[MAX_PATH];
      char temp_filename[MAX_PATH];
      GetTempPath(sizeof(temp_path), temp_path);
      GetTempFileName(temp_path, "growl-for-linux-", 0, temp_filename);
      FILE* fp = fopen(temp_filename, "wb");
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
        loader = (GdkPixbufLoader*) gdk_pixbuf_loader_new_with_mime_type(ctype, error);
      if (csize)
        size = atol(csize);
      if (!loader) loader = gdk_pixbuf_loader_new();
      if (body && gdk_pixbuf_loader_write(loader, (const guchar*) body, size, &_error))
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    }
    free(ctype);
    free(csize);
    if (loader) gdk_pixbuf_loader_close(loader, NULL);
  } else {
    _error = g_error_new_literal(G_FILE_ERROR, res, curl_easy_strerror(res));
  }

  free(head);
  free(body);

  /* cleanup callback data */
  if (error && _error) *error = _error;
  return pixbuf;
}

