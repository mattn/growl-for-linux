#include <string.h>
#include <stdbool.h>
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

  MEMFILE* body = memfopen();
  long code = 0;
  double csize = -1;
  char* ctype = NULL;

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(curl, CURLOPT_URL, info.url);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, info.body_writer);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  const CURLcode res = curl_easy_perform(curl);

  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &csize) != CURLE_OK)
      csize = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
  }

  if (info.code)  *info.code  = code;
  if (info.csize) *info.csize = csize;
  if (info.ctype) *info.ctype = ctype ? strdup(ctype) : NULL;
  if (info.body)  *info.body  = memfrelease(&body);
  memfclose(body);

  curl_easy_cleanup(curl);

  return res;
}

// Some error happened only if returns true.
static bool
gerror_set_or_free(GError** dest, GError* val) {
  if (!val) return false;

  if (dest) *dest = val;
  else g_error_free(val);
  return true;
}

// FIXME: More refactor this function.
static GdkPixbuf*
pixbuf_from_url_impl(const char* ctype, const MEMFILE* raw, GError** error) {
  GdkPixbuf* pixbuf = NULL;
#ifdef _WIN32
  if (ctype && (!strcmp(ctype, "image/jpeg") || !strcmp(ctype, "image/gif"))) {
    char temp_path[MAX_PATH];
    char temp_filename[MAX_PATH];
    GetTempPath(sizeof(temp_path), temp_path);
    GetTempFileName(temp_path, "growl-for-linux-", 0, temp_filename);
    FILE* const fp = fopen(temp_filename, "wb");
    if (fp) {
      fwrite(memfdata(raw), memfsize(raw), 1, fp);
      fclose(fp);
    }
    pixbuf = gdk_pixbuf_new_from_file(temp_filename, NULL);
    DeleteFile(temp_filename);
  } else
#endif
  {
    GError* _error = NULL;
    GdkPixbufLoader* const loader =
      ctype ? gdk_pixbuf_loader_new_with_mime_type(ctype, &_error)
            : gdk_pixbuf_loader_new();
    if (!gerror_set_or_free(error, _error)) {
      if (gdk_pixbuf_loader_write(loader, (const guchar*) memfcdata(raw), memfsize(raw), &_error))
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
      else
        gerror_set_or_free(error, _error);

      gdk_pixbuf_loader_close(loader, NULL);
    }
  }
  return pixbuf;
}

GdkPixbuf*
pixbuf_from_url(const char* url, GError** error) {
  if (!url) return NULL;
  if (!strncmp(url, "x-growl-resource://", 19)) {
    const gchar* const confdir = (const gchar*) g_get_user_config_dir();
    gchar* const resourcedir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", "resource", NULL);
    const gchar* const newurl = g_build_filename(resourcedir, url + 19, NULL);
    GdkPixbuf* pixbuf = pixbuf_from_url_as_file(newurl, error);
    g_free(resourcedir);
	return pixbuf;
  }

  MEMFILE* mbody;
  long code;
  double csize;
  char* ctype;
  const CURLcode res = memfile_from_url((memfile_from_url_info){
    .url         = url,
    .body        = &mbody,
    .body_writer = memfwrite,
    .code        = &code,
    .csize       = &csize,
    .ctype       = &ctype,
  });
  if (res != CURLE_OK || code != 200 || !mbody) {
    if (error) *error = g_error_new_literal(G_FILE_ERROR, res, curl_easy_strerror(res));
    free(ctype);
    memfclose(mbody);
    return NULL;
  }

  memfresize(mbody, csize >= 0 ? (size_t) csize : memfsize(mbody));

  GdkPixbuf* const pixbuf = pixbuf_from_url_impl(ctype, mbody, error);

  free(ctype);
  memfclose(mbody);

  return pixbuf;
}

GdkPixbuf*
pixbuf_from_url_as_file(const char* url, GError** error) {
  if (!url) return NULL;
  gchar* newurl;
  if (!strncmp(url, "x-growl-resource://", 19)) {
    const gchar* const confdir = (const gchar*) g_get_user_config_dir();
    gchar* const resourcedir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", "resource", NULL);
    newurl = g_build_filename(resourcedir, url + 19, NULL);
    g_free(resourcedir);
  } else
    newurl = g_filename_from_uri(url, NULL, NULL);
  GError* _error = NULL;
  GdkPixbuf* const pixbuf = gdk_pixbuf_new_from_file(newurl ? newurl : url, &_error);
  if (!pixbuf) gerror_set_or_free(error, _error);
  g_free(newurl);
  return pixbuf;
}

