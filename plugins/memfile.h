#ifndef plugins_memfile_h_
#define plugins_memfile_h_

typedef struct {
  char*  data;  // response data from server
  size_t size;  // response size of data
} MEMFILE;

MEMFILE*
memfopen();

void
memfclose(MEMFILE*);

size_t
memfwrite(char*, size_t, size_t, void*);

char*
memfstrdup(MEMFILE*);

#endif /* plugins_memfile_h_ */
