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

#ifdef _WIN32
# ifndef strncasecmp
#  define strncasecmp(d,s,n) strnicmp(d,s,n)
# endif
static char*
strndup(const char* src, size_t n) {
  char* ptr = (char*) malloc(n + 1);
  *(ptr + n) = 0;
  memcpy(ptr, src, n);
  return ptr;
}
#endif

#define REQUEST_TIMEOUT (5)

CURLcode
memfile_from_url(const memfile_from_url_info info) {
  CURL* curl = curl_easy_init();
  if (!curl) return CURLE_FAILED_INIT;

  MEMFILE* body = memfopen();
  MEMFILE* header = memfopen();

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(curl, CURLOPT_URL, info.url);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, info.body_writer);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, info.header_writer);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, header);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK) {
    if (info.body)   *info.body   = memfrelease(&body);
    if (info.header) *info.header = memfrelease(&header);
  }
  memfclose(body);
  memfclose(header);

  return res;
}

// remove leading spaces
static const char*
left_trim(const char* str) {
  while (*str && isspace(*str)) ++str;
  return *str ? str : NULL;
}

static char*
get_http_header_alloc(const char* ptr, const char* key) {
  if (!ptr || !key) return NULL;

  const size_t key_length = strlen(key);
  const char* term;
  for (; *ptr && (term = strpbrk(ptr, "\r\n")); ptr = term + 1) {
    if (ptr[key_length] == ':' && !strncasecmp(ptr, key, key_length))
      break;
  }
  const char* const top = left_trim(ptr + key_length + 1);
  return top ? strndup(top, (size_t) (term - top)) : NULL;
}

// Returns Content-Length field or defaults when no available.
static size_t
get_http_content_length(const char* ptr, const size_t defaults) {
  char* csize = get_http_header_alloc(ptr, "Content-Length");
  const size_t size = csize ? (size_t) atol(csize) : defaults;
  free(csize);
  return size;
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

  MEMFILE* mbody;
  MEMFILE* mhead;
  const CURLcode res = memfile_from_url((memfile_from_url_info){
    .url           = url,
    .body          = &mbody,
    .header        = &mhead,
    .body_writer   = memfwrite,
    .header_writer = memfwrite,
  });
  if (res != CURLE_OK || !mbody || !mhead) {
    if (error) *error = g_error_new_literal(G_FILE_ERROR, res, curl_easy_strerror(res));
    memfclose(mbody);
    memfclose(mhead);
    return NULL;
  }

  char* const head = memfstrdup(mhead);
  memfclose(mhead);

  char* const ctype = get_http_header_alloc(head, "Content-Type");
  memfresize(mbody, get_http_content_length(head, memfsize(mbody)));
  free(head);

  GdkPixbuf* const pixbuf = pixbuf_from_url_impl(ctype, mbody, error);

  free(ctype);
  memfclose(mbody);

  return pixbuf;
}

