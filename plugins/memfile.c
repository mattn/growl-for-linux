#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "compatibility.h"
#include "memfile.h"

MEMFILE*
memfopen() {
  return (MEMFILE*) calloc(1, sizeof(MEMFILE));
}

void
memfclose(MEMFILE* mf) {
  free(memfdata(mf));
  free(mf);
}

char*
memfresize(MEMFILE* mf, const size_t newsize) {
  if (!mf) return NULL;

  // Grow
  if (mf->size < newsize) {
    char* const tmp = (char*) realloc(mf->data, newsize);
    if (!tmp) return NULL;
    mf->data = tmp;
  }
  char* const insert_point = mf->data + mf->size;
  mf->size = newsize;
  return insert_point;
}

size_t
memfwrite(const char* ptr, size_t size, size_t nmemb, void* stream) {
  MEMFILE* const mf = (MEMFILE*) stream;
  const size_t block = size * nmemb;
  if (!mf) return block; // through

  const size_t orig_size = memfsize(mf);
  if (!memfresize(mf, orig_size + block)) return block;

  memcpy(memfdata(mf) + orig_size, ptr, block);
  return block;
}

char*
memfstrdup(const MEMFILE* mf) {
  if (!memfsize(mf)) return NULL;
  return strndup(memfcdata(mf), memfsize(mf));
}

