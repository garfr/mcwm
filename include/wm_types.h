#ifndef WM_TYPES_H
#define WM_TYPES_H

#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <stdbool.h>

/* Number of workspaces. */
#define WORKSPACES 10

/* All our key shortcuts. */
typedef enum
{
  KEY_F,
  KEY_H,
  KEY_J,
  KEY_K,
  KEY_L,
  KEY_M,
  KEY_R,
  KEY_RET,
  KEY_X,
  KEY_TAB,
  KEY_BACKTAB,
  KEY_1,
  KEY_2,
  KEY_3,
  KEY_4,
  KEY_5,
  KEY_6,
  KEY_7,
  KEY_8,
  KEY_9,
  KEY_0,
  KEY_Y,
  KEY_U,
  KEY_B,
  KEY_N,
  KEY_END,
  KEY_PREVSCR,
  KEY_NEXTSCR,
  KEY_ICONIFY,
  KEY_PREVWS,
  KEY_NEXTWS,
  KEY_MAX
} KeyKind;

typedef struct {
  xcb_randr_output_t id;
  char *name;
  int16_t x; /* X and Y. */
  int16_t y;
  uint16_t width;    /* Width in pixels. */
  uint16_t height;   /* Height in pixels. */
  struct item *item; /* Pointer to our place in output list. */
} Monitor;

typedef struct {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
} SizePos;

/* Everything we know about a window. */
typedef struct {
  xcb_drawable_t id;              /* ID of this window. */
  bool usercoord;                 /* X,Y was set by -geom. */
  int16_t x;                      /* X coordinate. */
  int16_t y;                      /* Y coordinate. */
  uint16_t width;                 /* Width in pixels. */
  uint16_t height;                /* Height in pixels. */
  SizePos origsize;               /* Original size if we're currently maxed. */
  uint16_t min_width, min_height; /* Hints from application. */
  uint16_t max_width, max_height;
  int32_t width_inc, height_inc;
  int32_t base_width, base_height;
  bool vertmaxed;       /* Vertically maximized? */
  bool maxed;           /* Totally maximized? */
  bool fixed;           /* Visible on all workspaces? */
  Monitor *monitor;     /* The physical output this window is on. */
  struct item *winitem; /* Pointer to our place in global windows list. */
  struct item *wsitem[WORKSPACES]; /* Pointer to our place in every
                                    * workspace window list. */
} Client;

/* Window configuration data. */
typedef struct {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  uint8_t stackmode;
  xcb_window_t sibling;
  uint16_t borderwidth;
} WinConf;

#endif
