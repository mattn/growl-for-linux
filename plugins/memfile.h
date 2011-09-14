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

