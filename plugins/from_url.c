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
