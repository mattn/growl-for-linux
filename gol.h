#ifndef _gol_h_
#define _gol_h_

typedef struct {
  gchar* title;
  gchar* text;
  gchar* icon;
  gchar* url;
  gint timeout;
} NOTIFICATION_INFO;

typedef struct {
  void (*show)(NOTIFICATION_INFO* ni);
} SUBSCRIPTOR_CONTEXT;

#endif /* _gol_h_ */
