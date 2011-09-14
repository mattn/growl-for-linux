#include <stdlib.h>
#include <string.h>

#include "memfile.h"

MEMFILE*
memfopen() {
  MEMFILE* mf = (MEMFILE*) malloc(sizeof(MEMFILE));
  if (mf) {
    mf->data = NULL;
    mf->size = 0;
  }
  return mf;
}

void
memfclose(MEMFILE* mf) {
  free(mf->data);
  free(mf);
}

size_t
memfwrite(const char* ptr, size_t size, size_t nmemb, void* stream) {
  MEMFILE* mf = (MEMFILE*) stream;
  const size_t block = size * nmemb;
  if (!mf) return block; // through
  if (!mf->data) {
    mf->data = (char*) malloc(block);
    mf->size = 0;
  } else {
    char* const tmp = (char*) realloc(mf->data, mf->size + block);
    if (tmp) mf->data = tmp;
    else return block; // through
  }
  if (mf->data) {
    memcpy(mf->data + mf->size, ptr, block);
    mf->size += block;
  }
  return block;
}

char*
memfstrdup(const MEMFILE* mf) {
  if (!mf || mf->size == 0) return NULL;
  char* buf = (char*) malloc(mf->size + 1);
  memcpy(buf, mf->data, mf->size);
  buf[mf->size] = 0;
  return buf;
}

