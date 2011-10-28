#ifndef plugins_memfile_h_
#define plugins_memfile_h_

#include <stddef.h>

#include "gol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char*  data;  // response data from server
  size_t size;  // response size of data
} MEMFILE;

GOL_INLINE const char*
memfcdata(const MEMFILE* mf) {
  return mf ? mf->data : NULL;
}

GOL_INLINE char*
memfdata(const MEMFILE* mf) {
  return mf ? mf->data : NULL;
}

GOL_INLINE size_t
memfsize(const MEMFILE* mf) {
  return mf ? mf->size : 0;
}

char*
memfresize(MEMFILE*, size_t);

GOL_INLINE MEMFILE*
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

