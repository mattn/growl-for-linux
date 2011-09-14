#ifndef plugins_memfile_h_
#define plugins_memfile_h_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char*  data;  // response data from server
  size_t size;  // response size of data
} MEMFILE;

static const char*
memfcdata(const MEMFILE* mf) {
  return mf ? mf->data : NULL;
}

static char*
memfdata(MEMFILE* mf) {
  return mf ? mf->data : NULL;
}

static size_t
memfsize(const MEMFILE* mf) {
  return mf ? mf->size : 0;
}

char*
memfresize(MEMFILE*, size_t);

static MEMFILE*
memfrelease(MEMFILE** pmf) {
  if (!pmf) return NULL;

  MEMFILE* const tmp = *pmf;
  *pmf = NULL;
  return tmp;
}

MEMFILE*
memfopen();

void
memfclose(MEMFILE*);

size_t
memfwrite(const char*, size_t, size_t, void*);

char*
memfstrdup(const MEMFILE*);

#ifdef __cplusplus
}
#endif

#endif /* plugins_memfile_h_ */

