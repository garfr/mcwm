/*
t * mcwm, a small window manager for the X Window System using the X
 * protocol C Binding libraries.
 *
 * For 'user' configurable stuff, see config.h.
 *
 * MC, mc at the domain hack.org
 * http://hack.org/mc/
 *
 * Copyright (c) 2010, 2011, 2012, 2013, 2014 Michael Cardell
 * Widerkrantz, mc at the domain hack.org.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#define _GNU_SOURCE
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_util.h>
#include <xcb/xproto.h>

#include "events.h"
#include "helpers.h"
#include "wm_types.h"

/* Check here for user configurable parts: */
#include "config.h"

/* Internal Constants. */

/* We're currently moving a window with the mouse. */
#define MCWM_MOVE 2

/* We're currently resizing a window with the mouse. */
#define MCWM_RESIZE 3

/*
 * We're currently tabbing around the window list, looking for a new
 * window to focus on.
 */
#define MCWM_TABBING 4

/* Value in WM hint which means this window is fixed on all workspaces. */
#define NET_WM_FIXED 0xffffffff

/* This means we didn't get any window hint at all. */
#define MCWM_NOWS 0xfffffffe

/* Globals */

int sigcode;                /* Signal code. Non-zero if we've been
                             * interruped by a signal. */
xcb_connection_t *conn;     /* Connection to X server. */
xcb_screen_t *screen;       /* Our current screen.  */
int randrbase;              /* Beginning of RANDR extension events. */
uint32_t curws = 0;         /* Current workspace. */
Client *focuswin;           /* Current focus window. */
Client *lastfocuswin;       /* Last focused window. NOTE! Only
                             * used to communicate between
                             * start and end of tabbing
                             * mode. */
LinkedNode *winlist = NULL; /* Global list of all client windows. */
LinkedNode *monlist = NULL; /* List of all physical monitor outputs. */
int mode = 0;               /* Internal mode, such as move or resize */

/*
 * Workspace list: Every workspace has a list of all visible
 * windows.
 */
LinkedNode *wslist[WORKSPACES] = {NULL, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL};

/* Shortcut key type and initializiation. */
struct keys {
  xcb_keysym_t keysym;
  xcb_keycode_t keycode;
} keys[KEY_MAX] = {
    {USERKEY_FIX, 0},        {USERKEY_MOVE_LEFT, 0},  {USERKEY_MOVE_DOWN, 0},
    {USERKEY_MOVE_UP, 0},    {USERKEY_MOVE_RIGHT, 0}, {USERKEY_MAXVERT, 0},
    {USERKEY_RAISE, 0},      {USERKEY_TERMINAL, 0},   {USERKEY_MAX, 0},
    {USERKEY_CHANGE, 0},     {USERKEY_BACKCHANGE, 0}, {USERKEY_WS1, 0},
    {USERKEY_WS2, 0},        {USERKEY_WS3, 0},        {USERKEY_WS4, 0},
    {USERKEY_WS5, 0},        {USERKEY_WS6, 0},        {USERKEY_WS7, 0},
    {USERKEY_WS8, 0},        {USERKEY_WS9, 0},        {USERKEY_WS10, 0},
    {USERKEY_TOPLEFT, 0},    {USERKEY_TOPRIGHT, 0},   {USERKEY_BOTLEFT, 0},
    {USERKEY_BOTRIGHT, 0},   {USERKEY_DELETE, 0},     {USERKEY_PREVSCREEN, 0},
    {USERKEY_NEXTSCREEN, 0}, {USERKEY_ICONIFY, 0},    {USERKEY_PREVWS, 0},
    {USERKEY_NEXTWS, 0},
};

/* All keycodes generating our MODKEY mask. */
struct modkeycodes {
  xcb_keycode_t *keycodes;
  unsigned len;
} modkeys = {NULL, 0};

/* Global configuration. */
struct conf {
  int borderwidth;     /* Do we draw borders? If so, how large? */
  int snapmargin;      /* Do we have snap margin? If so, how large? */
  char *terminal;      /* Path to terminal to start. */
  uint32_t focuscol;   /* Focused border colour. */
  uint32_t unfocuscol; /* Unfocused border colour.  */
  uint32_t fixedcol;   /* Fixed windows border colour. */
  bool allowicons;     /* Allow windows to be unmapped. */
} conf;

xcb_atom_t atom_desktop; /*
                          * EWMH _NET_WM_DESKTOP hint that says
                          * what workspace a window should be
                          * on.
                          */

xcb_atom_t wm_delete_window; /* WM_DELETE_WINDOW event to close windows.  */
xcb_atom_t wm_change_state;
xcb_atom_t wm_state;
xcb_atom_t wm_protocols; /* WM_PROTOCOLS.  */

/* Functions declerations. */

static void finishtabbing(void);
static struct modkeycodes getmodkeys(xcb_mod_mask_t modmask);
static void cleanup(int code);
static void arrangewindows(void);
static void setwmdesktop(xcb_drawable_t win, uint32_t ws);
static int32_t getwmdesktop(xcb_drawable_t win);
static void addtoworkspace(Client *client, uint32_t ws);
static void delfromworkspace(Client *client, uint32_t ws);
static void changeworkspace(uint32_t ws);
static void fixwindow(Client *client, bool setcolour);
static uint32_t getcolor(const char *colstr);
static void forgetclient(Client *client);
static void forgetwin(xcb_window_t win);
static void fitonscreen(Client *client);
static void newwin(xcb_window_t win);
static Client *setupwin(xcb_window_t win);
static xcb_keycode_t keysymtokeycode(xcb_keysym_t keysym,
                                     xcb_key_symbols_t *keysyms);
static int setupkeys(void);
static int setupscreen(void);
static int setuprandr(void);
static void getrandr(void);
static void getoutputs(xcb_randr_output_t *outputs, int len,
                       xcb_timestamp_t timestamp);
void arrbymon(Monitor *monitor);
static Monitor *findmonitor(xcb_randr_output_t id);
static Monitor *findclones(xcb_randr_output_t id, int16_t x, int16_t y);
static Monitor *findmonbycoord(int16_t x, int16_t y);
static void delmonitor(Monitor *mon);
static Monitor *addmonitor(xcb_randr_output_t id, char *name, uint32_t x,
                           uint32_t y, uint16_t width, uint16_t height);
static void raisewindow(xcb_drawable_t win);
static void raiseorlower(Client *client);
static void movelim(Client *client);
static void movewindow(xcb_drawable_t win, uint16_t x, uint16_t y);
static Client *findclient(xcb_drawable_t win);
static void focusnext(bool reverse);
static void setunfocus(xcb_drawable_t win);
static void setfocus(Client *client);
static int start(char *program);
static void resizelim(Client *client);
static void moveresize(xcb_drawable_t win, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height);
static void resize(xcb_drawable_t win, uint16_t width, uint16_t height);
static void resizestep(Client *client, char direction);
static void snapwindow(Client *client, int snap_mode);
static void mousemove(Client *client, int rel_x, int rel_y);
static void mouseresize(Client *client, int rel_x, int rel_y);
static void movestep(Client *client, char direction);
static void setborders(Client *client, int width);
static void unmax(Client *client);
static void maximize(Client *client);
static void maxvert(Client *client);
static void hide(Client *client);
static bool getpointer(xcb_drawable_t win, int16_t *x, int16_t *y);
static bool getgeom(xcb_drawable_t win, int16_t *x, int16_t *y, uint16_t *width,
                    uint16_t *height);
static void topleft(void);
static void topright(void);
static void botleft(void);
static void botright(void);
static void deletewin(void);
static void prevscreen(void);
static void nextscreen(void);
static void handle_keypress(xcb_key_press_event_t *ev);
static void configwin(xcb_window_t win, uint16_t mask, WinConf wc);
static void configurerequest(xcb_configure_request_event_t *e);
static void events(void);
static void printhelp(void);
static void sigcatch(int sig);
static xcb_atom_t getatom(char *atom_name);

/* Function bodies. */

/*
 * MODKEY was released after tabbing around the
 * workspace window ring. This means this mode is
 * finished and we have found a new focus window.
 *
 * We need to move first the window we used to focus
 * on to the head of the window list and then move the
 * new focus to the head of the list as well. The list
 * should always start with the window we're focusing
 * on.
 */
void
finishtabbing(void) {
  mode = 0;

  if (NULL != lastfocuswin) {
    list_move_head(&wslist[curws], lastfocuswin->wsitem[curws]);
    lastfocuswin = NULL;
  }

  list_move_head(&wslist[curws], focuswin->wsitem[curws]);
}

/*
 * Find out what keycode modmask is bound to. Returns a struct. If the
 * len in the struct is 0 something went wrong.
 */
struct modkeycodes
getmodkeys(xcb_mod_mask_t modmask) {
  xcb_get_modifier_mapping_cookie_t cookie;
  xcb_get_modifier_mapping_reply_t *reply;
  xcb_keycode_t *modmap;
  struct modkeycodes keycodes = {NULL, 0};
  int mask;
  unsigned i;
  const xcb_mod_mask_t masks[8] = {XCB_MOD_MASK_SHIFT,   XCB_MOD_MASK_LOCK,
                                   XCB_MOD_MASK_CONTROL, XCB_MOD_MASK_1,
                                   XCB_MOD_MASK_2,       XCB_MOD_MASK_3,
                                   XCB_MOD_MASK_4,       XCB_MOD_MASK_5};

  cookie = xcb_get_modifier_mapping_unchecked(conn);

  if ((reply = xcb_get_modifier_mapping_reply(conn, cookie, NULL)) == NULL) {
    return keycodes;
  }

  if (NULL == (keycodes.keycodes = calloc(reply->keycodes_per_modifier,
                                          sizeof(xcb_keycode_t)))) {
    log_err_final("out of memory");
    return keycodes;
  }

  modmap = xcb_get_modifier_mapping_keycodes(reply);

  /*
   * The modmap now contains keycodes.
   *
   * The number of keycodes in the list is 8 *
   * keycodes_per_modifier. The keycodes are divided into eight
   * sets, with each set containing keycodes_per_modifier elements.
   *
   * Each set corresponds to a modifier in masks[] in the order
   * specified above.
   *
   * The keycodes_per_modifier value is chosen arbitrarily by the
   * server. Zeroes are used to fill in unused elements within each
   * set.
   */
  for (mask = 0; mask < 8; mask++) {
    if (masks[mask] == modmask) {
      for (i = 0; i < reply->keycodes_per_modifier; i++) {
        if (0 != modmap[mask * reply->keycodes_per_modifier + i]) {
          keycodes.keycodes[i] =
              modmap[mask * reply->keycodes_per_modifier + i];
          keycodes.len++;
        }
      }

      log_verbose("Got %d keycodes", keycodes.len);
    }
  }

  free(reply);

  return keycodes;
}

/*
 * Set keyboard focus to follow mouse pointer. Then exit.
 *
 * We don't need to bother mapping all windows we know about. They
 * should all be in the X server's Save Set and should be mapped
 * automagically.
 */
void
cleanup(int code) {
  xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                      XCB_CURRENT_TIME);
  xcb_flush(conn);
  xcb_disconnect(conn);
  exit(code);
}

/*
 * Rearrange windows to fit new screen size.
 */
void
arrangewindows(void) {
  LinkedNode *item;
  Client *client;

  /*
   * Go through all windows. If they don't fit on the new screen,
   * move them around and resize them as necessary.
   */
  for (item = winlist; item != NULL; item = item->next) {
    client = item->data;
    fitonscreen(client);
  }
}

/* Set the EWMH hint that window win belongs on workspace ws. */
void
setwmdesktop(xcb_drawable_t win, uint32_t ws) {
  log_verbose("Changing _NET_WM_DESKTOP on window %d to %d", win, ws);

  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom_desktop,
                      XCB_ATOM_CARDINAL, 32, 1, &ws);
}

/*
 * Get EWWM hint so we might know what workspace window win should be
 * visible on.
 *
 * Returns either workspace, NET_WM_FIXED if this window should be
 * visible on all workspaces or MCWM_NOWS if we didn't find any hints.
 */
int32_t
getwmdesktop(xcb_drawable_t win) {
  xcb_get_property_reply_t *reply;
  xcb_get_property_cookie_t cookie;
  uint32_t *wsp;
  uint32_t ws;

  cookie = xcb_get_property(conn, false, win, atom_desktop,
                            XCB_GET_PROPERTY_TYPE_ANY, 0, sizeof(int32_t));

  reply = xcb_get_property_reply(conn, cookie, NULL);
  if (NULL == reply) {
    log_err("couldn't get properties for win %d", win);
    return MCWM_NOWS;
  }

  /* Length is 0 if we didn't find it. */
  if (0 == xcb_get_property_value_length(reply)) {
    log_verbose("_NET_WM_DESKTOP reply was 0 length");
    goto bad;
  }

  wsp = xcb_get_property_value(reply);

  ws = *wsp;

  log_verbose("got _NET_WM_DESKTOP: %d stored at %p", ws, (void *)wsp);

  free(reply);

  return ws;

bad:
  free(reply);
  return MCWM_NOWS;
}

/* Add a window, specified by client, to workspace ws. */
void
addtoworkspace(Client *client, uint32_t ws) {
  LinkedNode *item;

  item = list_add(&wslist[ws]);
  if (NULL == item) {
    log_err_final("out of memory");
    return;
  }

  /* Remember our place in the workspace window list. */
  client->wsitem[ws] = item;

  /* Remember the data. */
  item->data = client;

  /*
   * Set window hint property so we can survive a crash.
   *
   * Fixed windows have their own special WM hint. We don't want to
   * mess with that.
   */
  if (!client->fixed) {
    setwmdesktop(client->id, ws);
  }
}

/* Delete window client from workspace ws. */
void
delfromworkspace(Client *client, uint32_t ws) {
  list_del(&wslist[ws], client->wsitem[ws]);

  /* Reset our place in the workspace window list. */
  client->wsitem[ws] = NULL;
}

/* Change current workspace to ws. */
void
changeworkspace(uint32_t ws) {
  LinkedNode *item;
  Client *client;

  if (ws == curws) {
    log_verbose("changing to same workspace");
    return;
  }

  log_verbose("changing from workspace #%d to #%d", curws, ws);

  /*
   * We lose our focus if the window we focus isn't fixed. An
   * EnterNotify event will set focus later.
   */
  if (NULL != focuswin && !focuswin->fixed) {
    setunfocus(focuswin->id);
    focuswin = NULL;
  }

  /* Go through list of current ws. Unmap everything that isn't fixed. */
  for (item = wslist[curws]; item != NULL; item = item->next) {
    client = item->data;

    log_verbose("changeworkspace. unmap phase. ws #%d, client-fixed: %d", curws,
                client->fixed);

    if (!client->fixed) {
      /*
       * This is an ordinary window. Just unmap it. Note that
       * this will generate an unnecessary UnmapNotify event
       * which we will try to handle later.
       */
      xcb_unmap_window(conn, client->id);
    }
  }

  /* Go through list of new ws. Map everything that isn't fixed. */
  for (item = wslist[ws]; item != NULL; item = item->next) {
    client = item->data;

    log_verbose("changeworkspace. map phase. ws #%d, client-fixed: %d", ws,
                client->fixed);

    /* Fixed windows are already mapped. Map everything else. */
    if (!client->fixed) {
      xcb_map_window(conn, client->id);
    }
  }

  xcb_flush(conn);

  curws = ws;
}

/*
 * Fix or unfix a window client from all workspaces. If setcolour is
 * set, also change back to ordinary focus colour when unfixing.
 */
void
fixwindow(Client *client, bool setcolour) {
  uint32_t values[1];
  uint32_t ws;

  if (NULL == client) {
    return;
  }

  if (client->fixed) {
    client->fixed = false;
    setwmdesktop(client->id, curws);

    if (setcolour) {
      /* Set border color to ordinary focus colour. */
      values[0] = conf.focuscol;
      xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL,
                                   values);
    }

    /* Delete from all workspace lists except current. */
    for (ws = 0; ws < WORKSPACES; ws++) {
      if (ws != curws) {
        delfromworkspace(client, ws);
      }
    }
  } else {
    /*
     * First raise the window. If we're going to another desktop
     * we don't want this fixed window to be occluded behind
     * something else.
     */
    raisewindow(client->id);

    client->fixed = true;
    setwmdesktop(client->id, NET_WM_FIXED);

    /* Add window to all workspace lists. */
    for (ws = 0; ws < WORKSPACES; ws++) {
      if (ws != curws) {
        addtoworkspace(client, ws);
      }
    }

    if (setcolour) {
      /* Set border color to fixed colour. */
      values[0] = conf.fixedcol;
      xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL,
                                   values);
    }
  }

  xcb_flush(conn);
}

/*
 * Get the pixel values of a named colour colstr.
 *
 * Returns pixel values.
 * */
uint32_t
getcolor(const char *colstr) {
  xcb_alloc_named_color_reply_t *col_reply;
  xcb_colormap_t colormap;
  xcb_generic_error_t *error;
  xcb_alloc_named_color_cookie_t colcookie;

  colormap = screen->default_colormap;
  colcookie = xcb_alloc_named_color(conn, colormap, strlen(colstr), colstr);
  col_reply = xcb_alloc_named_color_reply(conn, colcookie, &error);
  if (NULL != error) {
    log_err_final("couldn't get pixel value for colour %s", colstr);
  }

  return col_reply->pixel;
}

/* Forget everything about client client. */
void
forgetclient(Client *client) {
  uint32_t ws;

  if (NULL == client) {
    log_verbose("forgetclient: client was NULL");
    return;
  }

  /*
   * Delete this client from whatever workspace lists it belongs to.
   * Note that it's OK to be on several workspaces at once even if
   * you're not fixed.
   */
  for (ws = 0; ws < WORKSPACES; ws++) {
    if (NULL != client->wsitem[ws]) {
      delfromworkspace(client, ws);
    }
  }

  /* Remove from global window list. */
  list_free(&winlist, NULL, client->winitem);
}

/* Forget everything about a client with client->id win. */
void
forgetwin(xcb_window_t win) {
  LinkedNode *item;
  Client *client;
  uint32_t ws;

  /* Find this window in the global window list. */
  for (item = winlist; item != NULL; item = item->next) {
    client = item->data;

    /*
     * Forget about it completely and free allocated data.
     *
     * Note that it might already be freed by handling an
     * UnmapNotify, so it isn't necessarily an error if we don't
     * find it.
     */
    log_verbose("win %d == client ID %d", win, client->id);
    if (win == client->id) {
      /* Found it. */
      log_verbose("found it");

      /*
       * Delete window from whatever workspace lists it belonged
       * to. Note that it's OK to be on several workspaces at
       * once.
       */
      for (ws = 0; ws < WORKSPACES; ws++) {
        log_verbose("looking in workspace #%d", ws);
        if (NULL == client->wsitem[ws]) {
          log_verbose("couldn't find workspace");
        } else {
          log_verbose("found in work space, delected");
          delfromworkspace(client, ws);
        }
      }

      free(item->data);
      list_del(&winlist, item);

      return;
    }
  }
}

/*
 * Fit client on physical screen, moving and resizing as necessary.
 */
void
fitonscreen(Client *client) {
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;
  bool willmove = false;
  bool willresize = false;

  client->vertmaxed = false;

  if (client->maxed) {
    client->maxed = false;
    setborders(client, conf.borderwidth);
  }

  if (NULL == client->monitor) {
    /*
     * This window isn't attached to any physical monitor. This
     * probably means there is no RANDR, so we use the root window
     * size.
     */
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = client->monitor->x;
    mon_y = client->monitor->y;
    mon_width = client->monitor->width;
    mon_height = client->monitor->height;
  }

  /* Is it outside the physical monitor? */
  if (client->x > mon_x + mon_width) {
    client->x = mon_x + mon_width - client->width;
    willmove = true;
  }
  if (client->y > mon_y + mon_height) {
    client->y = mon_y + mon_height - client->height;
    willmove = true;
  }

  if (client->x < mon_x) {
    client->x = mon_x;
    willmove = true;
  }
  if (client->y < mon_y) {
    client->y = mon_y;
    willmove = true;
  }

  /* Is it smaller than it wants to  be? */
  if (0 != client->min_height && client->height < client->min_height) {
    client->height = client->min_height;
    willresize = true;
  }

  if (0 != client->min_width && client->width < client->min_width) {
    client->width = client->min_width;
    willresize = true;
  }

  /*
   * If the window is larger than our screen, just place it in the
   * corner and resize.
   */
  if (client->width + conf.borderwidth * 2 > mon_width) {
    client->x = mon_x;
    client->width = mon_width - conf.borderwidth * 2;
    ;
    willmove = true;
    willresize = true;
  } else if (client->x + client->width + conf.borderwidth * 2 >
             mon_x + mon_width) {
    client->x = mon_x + mon_width - (client->width + conf.borderwidth * 2);
    willmove = true;
  }

  if (client->height + conf.borderwidth * 2 > mon_height) {
    client->y = mon_y;
    client->height = mon_height - conf.borderwidth * 2;
    willmove = true;
    willresize = true;
  } else if (client->y + client->height + conf.borderwidth * 2 >
             mon_y + mon_height) {
    client->y = mon_y + mon_height - (client->height + conf.borderwidth * 2);
    willmove = true;
  }

  if (willmove) {
    movewindow(client->id, client->x, client->y);
  }

  if (willresize) {
    resize(client->id, client->width, client->height);
  }
}

/*
 * Set position, geometry and attributes of a new window and show it
 * on the screen.
 */
void
newwin(xcb_window_t win) {
  Client *client;

  if (NULL != findclient(win)) {
    /*
     * We know this window from before. It's trying to map itself
     * on the current workspace, but since it's unmapped it
     * probably belongs on another workspace. We don't like that.
     * Silently ignore.
     */
    return;
  }

  /*
   * Set up stuff, like borders, add the window to the client list,
   * et cetera.
   */
  client = setupwin(win);
  if (NULL == client) {
    log_err_final("out of memory");
    return;
  }

  /* Add this window to the current workspace. */
  addtoworkspace(client, curws);

  /*
   * If the client doesn't say the user specified the coordinates
   * for the window we map it where our pointer is instead.
   */
  if (!client->usercoord) {
    int16_t pointx;
    int16_t pointy;

    /* Get pointer position so we can move the window to the cursor. */
    if (!getpointer(screen->root, &pointx, &pointy)) {
      log_verbose("failed to get pointer coords");
      pointx = 0;
      pointy = 0;
    }

    client->x = pointx;
    client->y = pointy;

    movewindow(client->id, client->x, client->y);
  } else {
    log_verbose("User set coordinates");
  }

  /* Find the physical output this window will be on if RANDR is active. */
  if (-1 != randrbase) {
    client->monitor = findmonbycoord(client->x, client->y);
    if (NULL == client->monitor) {
      /*
       * Window coordinates are outside all physical monitors.
       * Choose the first screen.
       */
      if (NULL != monlist) {
        client->monitor = monlist->data;
      }
    }
  }

  fitonscreen(client);

  /* Show window on screen. */
  xcb_map_window(conn, client->id);

  /* Declare window normal. */
  long data[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, wm_state,
                      wm_state, 32, 2, data);

  /*
   * Move cursor into the middle of the window so we don't lose the
   * pointer to another window.
   */
  xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0, client->width / 2,
                   client->height / 2);

  xcb_flush(conn);
}

/* Set border colour, width and event mask for window. */
Client *
setupwin(xcb_window_t win) {
  uint32_t mask = 0;
  uint32_t values[2];
  LinkedNode *item;
  Client *client;
  xcb_size_hints_t hints;
  uint32_t ws;

  /* Set default border color. */
  values[0] = conf.unfocuscol;
  xcb_change_window_attributes(conn, win, XCB_CW_BORDER_PIXEL, values);

  /* Subscribe to events we want to know about in this window. */
  mask = XCB_CW_EVENT_MASK;
  values[0] = XCB_EVENT_MASK_ENTER_WINDOW;
  xcb_change_window_attributes_checked(conn, win, mask, values);

  /*
   * Add this window to the X Save Set, that is, the windows that
   * will be automatically restored if we die.
   */
  xcb_change_save_set(conn, XCB_SET_MODE_INSERT, win);

  xcb_flush(conn);

  /* Remember window and store a few things about it. */

  item = list_add(&winlist);

  if (NULL == item) {
    log_err_final("out of memory");
    return NULL;
  }

  client = malloc(sizeof(Client));
  if (NULL == client) {
    log_err_final("out of memory");
    return NULL;
  }

  item->data = client;

  /* Initialize client. */
  client->id = win;
  client->usercoord = false;
  client->x = 0;
  client->y = 0;
  client->width = 0;
  client->height = 0;
  client->min_width = 0;
  client->min_height = 0;
  client->max_width = screen->width_in_pixels;
  client->max_height = screen->height_in_pixels;
  client->base_width = 0;
  client->base_height = 0;
  client->width_inc = 1;
  client->height_inc = 1;
  client->vertmaxed = false;
  client->maxed = false;
  client->fixed = false;
  client->monitor = NULL;

  client->winitem = item;

  for (ws = 0; ws < WORKSPACES; ws++) {
    client->wsitem[ws] = NULL;
  }

  log_verbose("adding window %d", client->id);

  setborders(client, conf.borderwidth);

  /* Get window geometry. */
  if (!getgeom(client->id, &client->x, &client->y, &client->width,
               &client->height)) {
    log_err_final("couldn't get geometry in initial setup of window");
  }

  /*
   * Get the window's incremental size step, if any.
   */
  if (!xcb_icccm_get_wm_normal_hints_reply(
          conn, xcb_icccm_get_wm_normal_hints_unchecked(conn, win), &hints,
          NULL)) {
    log_verbose("couldn't get size hints");
  }

  /*
   * The user specified the position coordinates. Remember that so
   * we can use geometry later.
   */
  if (hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) {
    client->usercoord = true;
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
    client->min_width = hints.min_width;
    client->min_height = hints.min_height;
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
    client->max_width = hints.max_width;
    client->max_height = hints.max_height;
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
    client->width_inc = hints.width_inc;
    client->height_inc = hints.height_inc;

    log_verbose("widht_inc %d, height_inc %d", client->width_inc,
                client->height_inc);
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
    client->base_width = hints.base_width;
    client->base_height = hints.base_height;
  }

  return client;
}

/*
 * Get a keycode from a keysym.
 *
 * Returns keycode value.
 */
xcb_keycode_t
keysymtokeycode(xcb_keysym_t keysym, xcb_key_symbols_t *keysyms) {
  xcb_keycode_t *keyp;
  xcb_keycode_t key;

  /* We only use the first keysymbol, even if there are more. */
  keyp = xcb_key_symbols_get_keycode(keysyms, keysym);
  if (NULL == keyp) {
    log_err_final("couldn't look up key");
    return 0;
  }

  key = *keyp;
  free(keyp);

  return key;
}

/*
 * Set up all shortcut keys.
 *
 * Returns 0 on success, non-zero otherwise.
 */
int
setupkeys(void) {
  xcb_key_symbols_t *keysyms;
  unsigned i;

  /* Get all the keysymbols. */
  keysyms = xcb_key_symbols_alloc(conn);

  /*
   * Find out what keys generates our MODKEY mask. Unfortunately it
   * might be several keys.
   */
  if (NULL != modkeys.keycodes) {
    free(modkeys.keycodes);
  }
  modkeys = getmodkeys(MODKEY);

  if (0 == modkeys.len) {
    return -1;
  }

  for (i = 0; i < modkeys.len; i++) {
    /*
     * Grab the keys that are bound to MODKEY mask with any other
     * modifier.
     */
    xcb_grab_key(conn, 1, screen->root, XCB_MOD_MASK_ANY, modkeys.keycodes[i],
                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  }

  /* Now grab the rest of the keys with the MODKEY modifier. */
  for (i = KEY_F; i < KEY_MAX; i++) {
    if (XK_VoidSymbol == keys[i].keysym) {
      keys[i].keycode = 0;
      continue;
    }

    keys[i].keycode = keysymtokeycode(keys[i].keysym, keysyms);
    if (0 == keys[i].keycode) {
      /* Couldn't set up keys! */

      /* Get rid of key symbols. */
      xcb_key_symbols_free(keysyms);

      return -1;
    }

    /* Grab other keys with a modifier mask. */
    xcb_grab_key(conn, 1, screen->root, MODKEY, keys[i].keycode,
                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    /*
     * XXX Also grab it's shifted counterpart. A bit ugly here
     * because we grab all of them not just the ones we want.
     */
    xcb_grab_key(conn, 1, screen->root, MODKEY | SHIFTMOD, keys[i].keycode,
                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  }

  /* Need this to take effect NOW! */
  xcb_flush(conn);

  /* Get rid of the key symbols table. */
  xcb_key_symbols_free(keysyms);

  return 0;
}

/*
 * Walk through all existing windows and set them up.
 *
 * Returns 0 on success.
 */
int
setupscreen(void) {
  xcb_query_tree_reply_t *reply;
  xcb_query_pointer_reply_t *pointer;
  int i;
  int len;
  xcb_window_t *children;
  xcb_get_window_attributes_reply_t *attr;
  Client *client;
  uint32_t ws;

  /* Get all children. */
  reply = xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), 0);
  if (NULL == reply) {
    return -1;
  }

  len = xcb_query_tree_children_length(reply);
  children = xcb_query_tree_children(reply);

  /* Set up all windows on this root. */
  for (i = 0; i < len; i++) {
    attr = xcb_get_window_attributes_reply(
        conn, xcb_get_window_attributes(conn, children[i]), NULL);

    if (!attr) {
      continue;
    }

    /*
     * Don't set up or even bother windows in override redirect
     * mode.
     *
     * This mode means they wouldn't have been reported to us
     * with a MapRequest if we had been running, so in the
     * normal case we wouldn't have seen them.
     *
     * Only handle visible windows.
     */
    if (!attr->override_redirect && attr->map_state == XCB_MAP_STATE_VIEWABLE) {
      client = setupwin(children[i]);
      if (NULL != client) {
        /*
         * Find the physical output this window will be on if
         * RANDR is active.
         */
        if (-1 != randrbase) {
          log_verbose("looking for monitor on %d x %d", client->x, client->y);
          client->monitor = findmonbycoord(client->x, client->y);
#if DEBUG
          if (NULL != client->monitor) {
            log_verbose("found client on monitor %s", client->monitor->name);
          } else {
            log_verbose("couldn't find client on any monitor");
          }
#endif
        }

        /* Fit window on physical screen. */
        fitonscreen(client);

        /*
         * Check if this window has a workspace set already as
         * a WM hint.
         *
         */
        ws = getwmdesktop(children[i]);

        if (ws == NET_WM_FIXED) {
          /* Add to current workspace. */
          addtoworkspace(client, curws);
          /* Add to all other workspaces. */
          fixwindow(client, false);
        } else if (MCWM_NOWS != ws && ws < WORKSPACES) {
          addtoworkspace(client, ws);
          /* If it's not our current workspace, hide it. */
          if (ws != curws) {
            xcb_unmap_window(conn, client->id);
          }
        } else {
          /*
           * No workspace hint at all. Just add it to our
           * current workspace.
           */
          addtoworkspace(client, curws);
        }
      }
    }

    free(attr);
  }

  changeworkspace(0);

  /*
   * Get pointer position so we can set focus on any window which
   * might be under it.
   */
  pointer =
      xcb_query_pointer_reply(conn, xcb_query_pointer(conn, screen->root), 0);

  if (NULL == pointer) {
    focuswin = NULL;
  } else {
    setfocus(findclient(pointer->child));
    free(pointer);
  }

  xcb_flush(conn);

  free(reply);

  return 0;
}

/*
 * Set up RANDR extension. Get the extension base and subscribe to
 * events.
 */
int
setuprandr(void) {
  const xcb_query_extension_reply_t *extension;
  int base;

  extension = xcb_get_extension_data(conn, &xcb_randr_id);
  if (!extension->present) {
    log_verbose("no RANDR extension");
    return -1;
  } else {
    getrandr();
  }

  base = extension->first_event;
  log_verbose("randrbase is %d", base);

  xcb_randr_select_input(conn, screen->root,
                         XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                             XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
                             XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                             XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);

  xcb_flush(conn);

  return base;
}

/*
 * Get RANDR resources and figure out how many outputs there are.
 */
void
getrandr(void) {
  xcb_randr_get_screen_resources_current_cookie_t rcookie;
  xcb_randr_get_screen_resources_current_reply_t *res;
  xcb_randr_output_t *outputs;
  int len;
  xcb_timestamp_t timestamp;

  rcookie = xcb_randr_get_screen_resources_current(conn, screen->root);
  res = xcb_randr_get_screen_resources_current_reply(conn, rcookie, NULL);
  if (NULL == res) {
    log_verbose("no RANDR extension available");
    return;
  }
  timestamp = res->config_timestamp;

  len = xcb_randr_get_screen_resources_current_outputs_length(res);
  outputs = xcb_randr_get_screen_resources_current_outputs(res);

  log_verbose("found %d outputs", len);

  /* Request information for all outputs. */
  getoutputs(outputs, len, timestamp);

  free(res);
}

/*
 * Walk through all the RANDR outputs (number of outputs == len) there
 * was at time timestamp.
 */
void
getoutputs(xcb_randr_output_t *outputs, int len, xcb_timestamp_t timestamp) {
  char *name;
  xcb_randr_get_crtc_info_cookie_t icookie;
  xcb_randr_get_crtc_info_reply_t *crtc = NULL;
  xcb_randr_get_output_info_reply_t *output;
  Monitor *mon;
  Monitor *clonemon;
  xcb_randr_get_output_info_cookie_t ocookie[len];
  int i;

  for (i = 0; i < len; i++) {
    ocookie[i] = xcb_randr_get_output_info(conn, outputs[i], timestamp);
  }

  /* Loop through all outputs. */
  for (i = 0; i < len; i++) {
    output = xcb_randr_get_output_info_reply(conn, ocookie[i], NULL);

    if (output == NULL) {
      continue;
    }

    asprintf(&name, "%.*s", xcb_randr_get_output_info_name_length(output),
             xcb_randr_get_output_info_name(output));

    log_verbose("name: %s, id: %d, size: %d x %d mm", name, outputs[i],
                output->mm_width, output->mm_height);

    if (XCB_NONE != output->crtc) {
      icookie = xcb_randr_get_crtc_info(conn, output->crtc, timestamp);
      crtc = xcb_randr_get_crtc_info_reply(conn, icookie, NULL);
      if (NULL == crtc) {
        return;
      }

      log_verbose("CRTC: at %d, %d, size: %d x %d", crtc->x, crtc->y,
                  crtc->width, crtc->height);

      /* Check if it's a clone. */
      clonemon = findclones(outputs[i], crtc->x, crtc->y);
      if (NULL != clonemon) {
        log_verbose("monitor %s, id %d is a clone of %s, id %d, skipping", name,
                    outputs[i], clonemon->name, clonemon->id);
        continue;
      }

      /* Do we know this monitor already? */
      if (NULL == (mon = findmonitor(outputs[i]))) {
        log_verbose("monitor not known, adding to list");
        addmonitor(outputs[i], name, crtc->x, crtc->y, crtc->width,
                   crtc->height);
      } else {
        bool changed = false;

        /*
         * We know this monitor. Update information. If it's
         * smaller than before, rearrange windows.
         */
        log_verbose("known monitor, updating info");

        if (crtc->x != mon->x) {
          mon->x = crtc->x;
          changed = true;
        }
        if (crtc->y != mon->y) {
          mon->y = crtc->y;
          changed = true;
        }

        if (crtc->width != mon->width) {
          mon->width = crtc->width;
          changed = true;
        }
        if (crtc->height != mon->height) {
          mon->height = crtc->height;
          changed = true;
        }

        if (changed) {
          arrbymon(mon);
        }
      }

      free(crtc);
    } else {
      log_verbose("monitor not used at the moment");
      /*
       * Check if it was used before. If it was, do something.
       */
      if ((mon = findmonitor(outputs[i]))) {
        LinkedNode *item;
        Client *client;

        /* Check all windows on this monitor and move them to
         * the next or to the first monitor if there is no
         * next.
         *
         * FIXME: Use per monitor workspace list instead of
         * global window list.
         */
        for (item = winlist; item != NULL; item = item->next) {
          client = item->data;
          if (client->monitor == mon) {
            if (NULL == client->monitor->item->next) {
              if (NULL == monlist) {
                client->monitor = NULL;
              } else {
                client->monitor = monlist->data;
              }
            } else {
              client->monitor = client->monitor->item->next->data;
            }

            fitonscreen(client);
          }
        }

        /* It's not active anymore. Forget about it. */
        delmonitor(mon);
      }
    }

    free(output);
  }
}

void
arrbymon(Monitor *monitor) {
  LinkedNode *item;
  Client *client;

  /*
   * Go through all windows on this monitor. If they don't fit on
   * the new screen, move them around and resize them as necessary.
   *
   * FIXME: Use a per monitor workspace list instead of global
   * windows list.
   */
  for (item = winlist; item != NULL; item = item->next) {
    client = item->data;
    if (client->monitor == monitor) {
      fitonscreen(client);
    }
  }
}

Monitor *
findmonitor(xcb_randr_output_t id) {
  LinkedNode *item;
  Monitor *mon;

  for (item = monlist; item != NULL; item = item->next) {
    mon = item->data;
    if (id == mon->id) {
      log_verbose("findmonitor: found it. output ID: %d", mon->id);
      return mon;
    }
  }

  return NULL;
}

Monitor *
findclones(xcb_randr_output_t id, int16_t x, int16_t y) {
  Monitor *clonemon;
  LinkedNode *item;

  for (item = monlist; item != NULL; item = item->next) {
    clonemon = item->data;

    log_verbose("monitor %s: x, y: %d--%d, %d--%d", clonemon->name, clonemon->x,
                clonemon->x + clonemon->width, clonemon->y,
                clonemon->y + clonemon->height);

    /* Check for same position. */
    if (id != clonemon->id && clonemon->x == x && clonemon->y == y) {
      return clonemon;
    }
  }

  return NULL;
}

Monitor *
findmonbycoord(int16_t x, int16_t y) {
  LinkedNode *item;
  Monitor *mon;

  for (item = monlist; item != NULL; item = item->next) {
    mon = item->data;

    if (x >= mon->x && x <= mon->x + mon->width && y >= mon->y &&
        y <= mon->y + mon->height) {
      log_verbose("findmonbycoord: found it. output ID: %d, name %s", mon->id,
                  mon->name);
      return mon;
    }
  }

  return NULL;
}

void
delmonitor(Monitor *mon) {
  log_verbose("deleting output %s", mon->name);
  free(mon->name);
  list_free(&monlist, NULL, mon->item);
}

Monitor *
addmonitor(xcb_randr_output_t id, char *name, uint32_t x, uint32_t y,
           uint16_t width, uint16_t height) {
  LinkedNode *item;
  Monitor *mon;

  if (NULL == (item = list_add(&monlist))) {
    log_err_final("out of memory");
    return NULL;
  }

  mon = malloc(sizeof(Monitor));
  if (NULL == mon) {
    log_err_final("out of memory");
    return NULL;
  }

  item->data = mon;

  mon->id = id;
  mon->name = name;
  mon->x = x;
  mon->y = y;
  mon->width = width;
  mon->height = height;
  mon->item = item;

  return mon;
}

/* Raise window win to top of stack. */
void
raisewindow(xcb_drawable_t win) {
  uint32_t values[] = {XCB_STACK_MODE_ABOVE};

  if (screen->root == win || 0 == win) {
    return;
  }

  xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);
  xcb_flush(conn);
}

/*
 * Set window client to either top or bottom of stack depending on
 * where it is now.
 */
void
raiseorlower(Client *client) {
  uint32_t values[] = {XCB_STACK_MODE_OPPOSITE};
  xcb_drawable_t win;

  if (NULL == client) {
    return;
  }

  win = client->id;

  xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);
  xcb_flush(conn);
}

void
movelim(Client *client) {
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  if (NULL == client->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = client->monitor->x;
    mon_y = client->monitor->y;
    mon_width = client->monitor->width;
    mon_height = client->monitor->height;
  }

  /* Is it outside the physical monitor? */
  if (client->x < mon_x) {
    client->x = mon_x;
  }
  if (client->y < mon_y) {
    client->y = mon_y;
  }

  if (client->x + client->width > mon_x + mon_width - conf.borderwidth * 2) {
    client->x = (mon_x + mon_width - conf.borderwidth * 2) - client->width;
  }

  if (client->y + client->height > mon_y + mon_height - conf.borderwidth * 2) {
    client->y = (mon_y + mon_height - conf.borderwidth * 2) - client->height;
  }

  movewindow(client->id, client->x, client->y);
}

/* Move window win to root coordinates x,y. */
void
movewindow(xcb_drawable_t win, uint16_t x, uint16_t y) {
  uint32_t values[2];

  if (screen->root == win || 0 == win) {
    /* Can't move root. */
    return;
  }

  values[0] = x;
  values[1] = y;

  xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                       values);

  xcb_flush(conn);
}

/* Change focus to next in window ring. */
void
focusnext(bool reverse) {
  Client *client = NULL;

#if DEBUG
  if (NULL != focuswin) {
    log_verbose("focus now in win %d", focuswin->id);
  }
#endif

  if (NULL == wslist[curws]) {
    log_verbose("no windows to focus on in this workspace");
    return;
  }

  if (MCWM_TABBING != mode) {
    /*
     * Remember what we last focused on. We need this when the
     * MODKEY is released and we move the last focused window in
     * the tabbing order list.
     */
    lastfocuswin = focuswin;
    mode = MCWM_TABBING;

    log_verbose("began tabbing");
  }

  /* If we currently have no focus focus first in list. */
  if (NULL == focuswin || NULL == focuswin->wsitem[curws]) {
    client = wslist[curws]->data;
  } else {
    if (reverse) {
      if (NULL == focuswin->wsitem[curws]->prev) {
        /*
         * We were at the head of list. Focusing on last
         * window in list unless we were already there.
         */
        LinkedNode *last = wslist[curws];
        while (NULL != last->next)
          last = last->next;
        if (focuswin->wsitem[curws] != last->data) {
          client = last->data;
        }
      } else {
        /* Otherwise, focus the next in list. */
        client = focuswin->wsitem[curws]->prev->data;
      }
    } else {
      if (NULL == focuswin->wsitem[curws]->next) {
        /*
         * We were at the end of list. Focusing on first window in
         * list unless we were already there.
         */
        if (focuswin->wsitem[curws] != wslist[curws]->data) {
          client = wslist[curws]->data;
        }
      } else {
        /* Otherwise, focus the next in list. */
        client = focuswin->wsitem[curws]->next->data;
      }
    }
  }

  if (NULL != client) {
    /*
     * Raise window if it's occluded, then warp pointer into it and
     * set keyboard focus to it.
     */
    uint32_t values[] = {XCB_STACK_MODE_TOP_IF};

    xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_STACK_MODE,
                         values);
    xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, client->width / 2,
                     client->height / 2);
    setfocus(client);
  }
}

/* Mark window win as unfocused. */
void
setunfocus(xcb_drawable_t win) {
  uint32_t values[1];

  if (NULL == focuswin) {
    return;
  }

  if (focuswin->id == screen->root) {
    return;
  }

  /* Set new border colour. */
  values[0] = conf.unfocuscol;
  xcb_change_window_attributes(conn, win, XCB_CW_BORDER_PIXEL, values);

  xcb_flush(conn);
}

/*
 * Find client with client->id win in global window list.
 *
 * Returns client pointer or NULL if not found.
 */
Client *
findclient(xcb_drawable_t win) {
  LinkedNode *item;
  Client *client;

  for (item = winlist; item != NULL; item = item->next) {
    client = item->data;
    if (win == client->id) {
      return client;
    }
  }

  return NULL;
}

/* Set focus on window client. */
void
setfocus(Client *client) {
  uint32_t values[1];

  /*
   * If client is NULL, we focus on whatever the pointer is on.
   *
   * This is a pathological case, but it will make the poor user
   * able to focus on windows anyway, even though this window
   * manager might be buggy.
   */
  if (NULL == client) {

    focuswin = NULL;

    xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                        XCB_CURRENT_TIME);
    xcb_flush(conn);

    return;
  }

  /*
   * Don't bother focusing on the root window or on the same window
   * that already has focus.
   */
  if (client->id == screen->root || client == focuswin) {
    return;
  }

  /* Set new border colour. */
  if (client->fixed) {
    values[0] = conf.fixedcol;
  } else {
    values[0] = conf.focuscol;
  }

  xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL, values);

  /* Unset last focus. */
  if (NULL != focuswin) {
    setunfocus(focuswin->id);
  }

  /* Set new input focus. */

  xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, client->id,
                      XCB_CURRENT_TIME);

  xcb_flush(conn);

  /* Remember the new window as the current focused window. */
  focuswin = client;
}

int
start(char *program) {
  pid_t pid;

  pid = fork();
  if (-1 == pid) {
    perror("fork");
    return -1;
  } else if (0 == pid) {
    char *argv[2];

    /* In the child. */

    /*
     * Make this process a new process leader, otherwise the
     * terminal will die when the wm dies. Also, this makes any
     * SIGCHLD go to this process when we fork again.
     */
    if (-1 == setsid()) {
      perror("setsid");
      exit(1);
    }

    argv[0] = program;
    argv[1] = NULL;

    if (-1 == execvp(program, argv)) {
      perror("execve");
      exit(1);
    }
    exit(0);
  }

  return 0;
}

/* Resize with limit. */
void
resizelim(Client *client) {
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  if (NULL == client->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = client->monitor->x;
    mon_y = client->monitor->y;
    mon_width = client->monitor->width;
    mon_height = client->monitor->height;
  }

  /* Is it smaller than it wants to  be? */
  if (0 != client->min_height && client->height < client->min_height) {
    client->height = client->min_height;
  }

  if (0 != client->min_width && client->width < client->min_width) {
    client->width = client->min_width;
  }

  if (client->x + client->width + conf.borderwidth * 2 > mon_x + mon_width) {
    client->width = mon_width - ((client->x - mon_x) + conf.borderwidth * 2);
  }

  if (client->y + client->height + conf.borderwidth * 2 > mon_y + mon_height) {
    client->height = mon_height - ((client->y - mon_y) + conf.borderwidth * 2);
  }

  resize(client->id, client->width, client->height);
}

void
moveresize(xcb_drawable_t win, uint16_t x, uint16_t y, uint16_t width,
           uint16_t height) {
  uint32_t values[4];

  if (screen->root == win || 0 == win) {
    /* Can't move or resize root. */
    return;
  }

  values[0] = x;
  values[1] = y;
  values[2] = width;
  values[3] = height;

  xcb_configure_window(conn, win,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                           XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);
  xcb_flush(conn);
}

/* Resize window win to width,height. */
void
resize(xcb_drawable_t win, uint16_t width, uint16_t height) {
  uint32_t values[2];

  if (screen->root == win || 0 == win) {
    /* Can't resize root. */
    return;
  }

  values[0] = width;
  values[1] = height;

  xcb_configure_window(
      conn, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
  xcb_flush(conn);
}

/*
 * Resize window client in direction direction. Direction is:
 *
 * h = left, that is decrease width.
 *
 * j = down, that is, increase height.
 *
 * k = up, that is, decrease height.
 *
 * l = right, that is, increase width.
 */
void
resizestep(Client *client, char direction) {
  int step_x = MOVE_STEP;
  int step_y = MOVE_STEP;

  if (NULL == client) {
    return;
  }

  if (client->maxed) {
    /* Can't resize a fully maximized window. */
    return;
  }

  raisewindow(client->id);

  if (client->width_inc > 1) {
    step_x = client->width_inc;
  } else {
    step_x = MOVE_STEP;
  }

  if (client->height_inc > 1) {
    step_y = client->height_inc;
  } else {
    step_y = MOVE_STEP;
  }

  switch (direction) {
    case 'h':
      client->width = client->width - step_x;
      break;

    case 'j':
      client->height = client->height + step_y;
      break;

    case 'k':
      client->height = client->height - step_y;
      break;

    case 'l':
      client->width = client->width + step_x;
      break;

    default:
      break;
  } /* switch direction */

  resizelim(client);

  /* If this window was vertically maximized, remember that it isn't now. */
  if (client->vertmaxed) {
    client->vertmaxed = false;
  }

  xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, client->width / 2,
                   client->height / 2);
  xcb_flush(conn);
}

/*
 * Try to snap to other windows and monitor border
 */
static void
snapwindow(Client *client, int snap_mode) {
  LinkedNode *item;
  Client *win;
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  if (NULL == client->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = client->monitor->x;
    mon_y = client->monitor->y;
    mon_width = client->monitor->width;
    mon_height = client->monitor->height;
  }
  /*
   * Go through all windows on current workspace.
   */
  for (item = wslist[curws]; item != NULL; item = item->next) {
    win = item->data;

    if (client == win) {
      continue;
    }

    if (snap_mode == MCWM_MOVE) {
      if (abs((win->x + win->width) - client->x) < conf.snapmargin) {
        if (client->y + client->height > win->y &&
            client->y < win->y + win->height) {
          client->x = (win->x + win->width) + (2 * conf.borderwidth);
        }
      }

      if (abs((win->y + win->height) - client->y) < conf.snapmargin) {
        if (client->x + client->width > win->x &&
            client->x < win->x + win->width) {
          client->y = (win->y + win->height) + (2 * conf.borderwidth);
        }
      }

      if (abs((client->x + client->width) - win->x) < conf.snapmargin) {
        if (client->y + client->height > win->y &&
            client->y < win->y + win->height) {
          client->x = (win->x - client->width) - (2 * conf.borderwidth);
        }
      }

      if (abs((client->y + client->height) - win->y) < conf.snapmargin) {
        if (client->x + client->width > win->x &&
            client->x < win->x + win->width) {
          client->y = (win->y - client->height) - (2 * conf.borderwidth);
        }
      }

    } /* mcwm_move */
    else if (snap_mode == MCWM_RESIZE) {
      if (abs((client->x + client->width) - win->x) < conf.snapmargin) {
        if (client->y + client->height > win->y &&
            client->y < win->y + win->height) {
          client->width = (win->x - client->x) - (2 * conf.borderwidth);
        }
      }

      if (abs((client->y + client->height) - win->y) < conf.snapmargin) {
        if (client->x + client->width > win->x &&
            client->x < win->x + win->width) {
          client->height = (win->y - client->y) - (2 * conf.borderwidth);
        }
      }
    } /* mcwm_resize */
  }

  /* monitor border */
  if (snap_mode == MCWM_MOVE) {
    if (abs(client->x - mon_x) < conf.snapmargin) {
      client->x = mon_x;
    }

    if (abs(client->y - mon_y) < conf.snapmargin) {
      client->y = mon_y;
    }

    if (abs((client->x + client->width) - mon_width) < conf.snapmargin) {
      client->x = (mon_width - client->width);
    }

    if (abs((client->y + client->height) - mon_height) < conf.snapmargin) {
      client->y = (mon_height - client->height);
    }
  } else if (snap_mode == MCWM_RESIZE) {
    if (abs((client->width + client->x) - mon_width) < conf.snapmargin) {
      client->width = mon_width;
    }
    if (abs((client->height + client->y) - mon_height) < conf.snapmargin) {
      client->height = mon_height;
    }
  }
}

/*
 * Move window win as a result of pointer motion to coordinates
 * rel_x,rel_y.
 */
void
mousemove(Client *client, int rel_x, int rel_y) {
  client->x = rel_x;
  client->y = rel_y;

  if (conf.snapmargin > 0) {
    snapwindow(client, MCWM_MOVE);
  }

  movelim(client);
}

void
mouseresize(Client *client, int rel_x, int rel_y) {
  client->width = abs(rel_x - client->x);
  client->height = abs(rel_y - client->y);

  client->width -= (client->width - client->base_width) % client->width_inc;
  client->height -= (client->height - client->base_height) % client->height_inc;

  log_verbose("Trying to resize to %dx%d (%dx%d)", client->width,
              client->height,
              (client->width - client->base_width) / client->width_inc,
              (client->height - client->base_height) / client->height_inc);

  if (conf.snapmargin > 0) {
    snapwindow(client, MCWM_RESIZE);
  }

  resizelim(client);

  /* If this window was vertically maximized, remember that it isn't now. */
  if (client->vertmaxed) {
    client->vertmaxed = false;
  }
}

void
movestep(Client *client, char direction) {
  int16_t start_x;
  int16_t start_y;

  if (NULL == client) {
    return;
  }

  if (client->maxed) {
    /* We can't move a fully maximized window. */
    return;
  }

  /* Save pointer position so we can warp pointer here later. */
  if (!getpointer(client->id, &start_x, &start_y)) {
    return;
  }

  raisewindow(client->id);
  switch (direction) {
    case 'h':
      client->x = client->x - MOVE_STEP;
      break;

    case 'j':
      client->y = client->y + MOVE_STEP;
      break;

    case 'k':
      client->y = client->y - MOVE_STEP;
      break;

    case 'l':
      client->x = client->x + MOVE_STEP;
      break;

    default:
      break;
  } /* switch direction */

  movelim(client);

  /*
   * If the pointer was inside the window to begin with, move
   * pointer back to where it was, relative to the window.
   */
  if (start_x > 0 - conf.borderwidth &&
      start_x < client->width + conf.borderwidth &&
      start_y > 0 - conf.borderwidth &&
      start_y < client->height + conf.borderwidth) {
    xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, start_x, start_y);
    xcb_flush(conn);
  }
}

void
setborders(Client *client, int width) {
  uint32_t values[1];
  uint32_t mask = 0;

  values[0] = width;

  mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
  xcb_configure_window(conn, client->id, mask, &values[0]);
  xcb_flush(conn);
}

void
unmax(Client *client) {
  uint32_t values[5];
  uint32_t mask = 0;

  if (NULL == client) {
    return;
  }

  client->x = client->origsize.x;
  client->y = client->origsize.y;
  client->width = client->origsize.width;
  client->height = client->origsize.height;

  /* Restore geometry. */
  if (client->maxed) {
    values[0] = client->x;
    values[1] = client->y;
    values[2] = client->width;
    values[3] = client->height;

    /* Set borders again. */
    values[4] = conf.borderwidth;

    mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
           XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH;
  } else {
    values[0] = client->y;
    values[1] = client->width;
    values[2] = client->height;

    mask = XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
           XCB_CONFIG_WINDOW_HEIGHT;
  }

  xcb_configure_window(conn, client->id, mask, values);

  /* Warp pointer to window or we might lose it. */
  xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, client->width / 2,
                   client->height / 2);

  xcb_flush(conn);
}

void
maximize(Client *client) {
  uint32_t values[4];
  uint32_t mask = 0;
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  if (NULL == client) {
    return;
  }

  if (NULL == client->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = client->monitor->x;
    mon_y = client->monitor->y;
    mon_width = client->monitor->width;
    mon_height = client->monitor->height;
  }

  /*
   * Check if maximized already. If so, revert to stored
   * geometry.
   */
  if (client->maxed) {
    unmax(client);
    client->maxed = false;
    return;
  }

  /* Raise first. Pretty silly to maximize below something else. */
  raisewindow(client->id);

  /* FIXME: Store original geom in property as well? */
  client->origsize.x = client->x;
  client->origsize.y = client->y;
  client->origsize.width = client->width;
  client->origsize.height = client->height;

  /* Remove borders. */
  values[0] = 0;
  mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
  xcb_configure_window(conn, client->id, mask, values);

  /* Move to top left and resize. */
  client->x = mon_x;
  client->y = mon_y;
  client->width = mon_width;
  client->height = mon_height;

  values[0] = client->x;
  values[1] = client->y;
  values[2] = client->width;
  values[3] = client->height;
  xcb_configure_window(conn, client->id,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                           XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);

  xcb_flush(conn);

  client->maxed = true;
}

void
maxvert(Client *client) {
  uint32_t values[2];
  int16_t mon_y;
  uint16_t mon_height;

  if (NULL == client) {
    return;
  }

  if (NULL == client->monitor) {
    mon_y = 0;
    mon_height = screen->height_in_pixels;
  } else {
    mon_y = client->monitor->y;
    mon_height = client->monitor->height;
  }

  /*
   * Check if maximized already. If so, revert to stored geometry.
   */
  if (client->vertmaxed) {
    unmax(client);
    client->vertmaxed = false;
    return;
  }

  /* Raise first. Pretty silly to maximize below something else. */
  raisewindow(client->id);

  /*
   * Store original coordinates and geometry.
   * FIXME: Store in property as well?
   */
  client->origsize.x = client->x;
  client->origsize.y = client->y;
  client->origsize.width = client->width;
  client->origsize.height = client->height;

  client->y = mon_y;
  /* Compute new height considering height increments and screen height. */
  client->height = mon_height - conf.borderwidth * 2;
  client->height -= (client->height - client->base_height) % client->height_inc;

  /* Move to top of screen and resize. */
  values[0] = client->y;
  values[1] = client->height;

  xcb_configure_window(conn, client->id,
                       XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);
  xcb_flush(conn);

  /* Remember that this client is vertically maximized. */
  client->vertmaxed = true;
}

void
hide(Client *client) {
  long data[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};

  /*
   * Unmap window and declare iconic.
   *
   * Unmapping will generate an UnmapNotify event so we can forget
   * about the window later.
   */
  xcb_unmap_window(conn, client->id);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->id, wm_state,
                      wm_state, 32, 2, data);
  xcb_flush(conn);
}

bool
getpointer(xcb_drawable_t win, int16_t *x, int16_t *y) {
  xcb_query_pointer_reply_t *pointer;

  pointer = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, win), 0);
  if (NULL == pointer) {
    return false;
  }

  *x = pointer->win_x;
  *y = pointer->win_y;

  free(pointer);

  return true;
}

bool
getgeom(xcb_drawable_t win, int16_t *x, int16_t *y, uint16_t *width,
        uint16_t *height) {
  xcb_get_geometry_reply_t *geom;

  geom = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL);
  if (NULL == geom) {
    return false;
  }

  *x = geom->x;
  *y = geom->y;
  *width = geom->width;
  *height = geom->height;

  free(geom);

  return true;
}

void
topleft(void) {
  int16_t pointx;
  int16_t pointy;
  int16_t mon_x;
  int16_t mon_y;

  if (NULL == focuswin) {
    return;
  }

  if (NULL == focuswin->monitor) {
    mon_x = 0;
    mon_y = 0;
  } else {
    mon_x = focuswin->monitor->x;
    mon_y = focuswin->monitor->y;
  }

  raisewindow(focuswin->id);

  if (!getpointer(focuswin->id, &pointx, &pointy)) {
    return;
  }

  focuswin->x = mon_x;
  focuswin->y = mon_y;
  movewindow(focuswin->id, focuswin->x, focuswin->y);
  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, pointx, pointy);
  xcb_flush(conn);
}

void
topright(void) {
  int16_t pointx;
  int16_t pointy;
  int16_t mon_x;
  uint16_t mon_y;
  uint16_t mon_width;

  if (NULL == focuswin) {
    return;
  }

  if (NULL == focuswin->monitor) {
    mon_width = screen->width_in_pixels;
    mon_x = 0;
    mon_y = 0;
  } else {
    mon_width = focuswin->monitor->width;
    mon_x = focuswin->monitor->x;
    mon_y = focuswin->monitor->y;
  }

  raisewindow(focuswin->id);

  if (!getpointer(focuswin->id, &pointx, &pointy)) {
    return;
  }

  focuswin->x = mon_x + mon_width - (focuswin->width + conf.borderwidth * 2);

  focuswin->y = mon_y;

  movewindow(focuswin->id, focuswin->x, focuswin->y);

  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, pointx, pointy);
  xcb_flush(conn);
}

void
botleft(void) {
  int16_t pointx;
  int16_t pointy;
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_height;

  if (NULL == focuswin) {
    return;
  }

  if (NULL == focuswin->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = focuswin->monitor->x;
    mon_y = focuswin->monitor->y;
    mon_height = focuswin->monitor->height;
  }

  raisewindow(focuswin->id);

  if (!getpointer(focuswin->id, &pointx, &pointy)) {
    return;
  }

  focuswin->x = mon_x;
  focuswin->y = mon_y + mon_height - (focuswin->height + conf.borderwidth * 2);
  movewindow(focuswin->id, focuswin->x, focuswin->y);
  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, pointx, pointy);
  xcb_flush(conn);
}

void
botright(void) {
  int16_t pointx;
  int16_t pointy;
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  if (NULL == focuswin) {
    return;
  }

  if (NULL == focuswin->monitor) {
    mon_x = 0;
    mon_y = 0;
    mon_width = screen->width_in_pixels;
    ;
    mon_height = screen->height_in_pixels;
  } else {
    mon_x = focuswin->monitor->x;
    mon_y = focuswin->monitor->y;
    mon_width = focuswin->monitor->width;
    mon_height = focuswin->monitor->height;
  }

  raisewindow(focuswin->id);

  if (!getpointer(focuswin->id, &pointx, &pointy)) {
    return;
  }

  focuswin->x = mon_x + mon_width - (focuswin->width + conf.borderwidth * 2);
  focuswin->y = mon_y + mon_height - (focuswin->height + conf.borderwidth * 2);
  movewindow(focuswin->id, focuswin->x, focuswin->y);
  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, pointx, pointy);
  xcb_flush(conn);
}

void
deletewin(void) {
  xcb_get_property_cookie_t cookie;
  xcb_icccm_get_wm_protocols_reply_t protocols;
  bool use_delete = false;
  uint32_t i;

  if (NULL == focuswin) {
    return;
  }

  /* Check if WM_DELETE is supported.  */
  cookie =
      xcb_icccm_get_wm_protocols_unchecked(conn, focuswin->id, wm_protocols);
  if (xcb_icccm_get_wm_protocols_reply(conn, cookie, &protocols, NULL) == 1) {
    for (i = 0; i < protocols.atoms_len; i++) {
      if (protocols.atoms[i] == wm_delete_window) {
        use_delete = true;
      }
    }
  }

  xcb_icccm_get_wm_protocols_reply_wipe(&protocols);

  if (use_delete) {
    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .sequence = 0,
        .window = focuswin->id,
        .type = wm_protocols,
        .data.data32 = {wm_delete_window, XCB_CURRENT_TIME}};

    xcb_send_event(conn, false, focuswin->id, XCB_EVENT_MASK_NO_EVENT,
                   (char *)&ev);
  } else {
    xcb_kill_client(conn, focuswin->id);
  }

  xcb_flush(conn);
}

void
prevscreen(void) {
  LinkedNode *item;

  if (NULL == focuswin || NULL == focuswin->monitor) {
    return;
  }

  item = focuswin->monitor->item->prev;

  if (NULL == item) {
    return;
  }

  focuswin->monitor = item->data;

  raisewindow(focuswin->id);
  fitonscreen(focuswin);
  movelim(focuswin);

  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, 0, 0);
  xcb_flush(conn);
}

void
nextscreen(void) {
  LinkedNode *item;

  if (NULL == focuswin || NULL == focuswin->monitor) {
    return;
  }

  item = focuswin->monitor->item->next;

  if (NULL == item) {
    return;
  }

  focuswin->monitor = item->data;

  raisewindow(focuswin->id);
  fitonscreen(focuswin);
  movelim(focuswin);

  xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, 0, 0);
  xcb_flush(conn);
}

void
handle_keypress(xcb_key_press_event_t *ev) {
  int i;
  KeyKind key;

  for (key = KEY_MAX, i = KEY_F; i < KEY_MAX; i++) {
    if (ev->detail == keys[i].keycode && 0 != keys[i].keycode) {
      key = i;
      break;
    }
  }
  if (key == KEY_MAX) {
    log_verbose("unknown key pressed");

    /*
     * We don't know what to do with this key. Send this key press
     * event to the focused window.
     */
    xcb_send_event(conn, false, XCB_SEND_EVENT_DEST_ITEM_FOCUS,
                   XCB_EVENT_MASK_NO_EVENT, (char *)ev);
    xcb_flush(conn);
    return;
  }

  if (MCWM_TABBING == mode && key != KEY_TAB && key != KEY_BACKTAB) {
    /* First finish tabbing around. Then deal with the next key. */
    finishtabbing();
  }

  /* Is it shifted? */
  if (ev->state & SHIFTMOD) {
    switch (key) {
      case KEY_H: /* h */
        resizestep(focuswin, 'h');
        break;

      case KEY_J: /* j */
        resizestep(focuswin, 'j');
        break;

      case KEY_K: /* k */
        resizestep(focuswin, 'k');
        break;

      case KEY_L: /* l */
        resizestep(focuswin, 'l');
        break;

      case KEY_TAB: /* shifted tab counts as backtab */
        focusnext(true);
        break;

      default:
        /* Ignore other shifted keys. */
        break;
    }
  } else {
    switch (key) {
      case KEY_RET: /* return */
        start(conf.terminal);
        break;

      case KEY_F: /* f */
        fixwindow(focuswin, true);
        break;

      case KEY_H: /* h */
        movestep(focuswin, 'h');
        break;

      case KEY_J: /* j */
        movestep(focuswin, 'j');
        break;

      case KEY_K: /* k */
        movestep(focuswin, 'k');
        break;

      case KEY_L: /* l */
        movestep(focuswin, 'l');
        break;

      case KEY_TAB: /* tab */
        focusnext(false);
        break;

      case KEY_BACKTAB: /* backtab */
        focusnext(true);
        break;

      case KEY_M: /* m */
        maxvert(focuswin);
        break;

      case KEY_R: /* r*/
        raiseorlower(focuswin);
        break;

      case KEY_X: /* x */
        maximize(focuswin);
        break;

      case KEY_1:
        changeworkspace(0);
        break;

      case KEY_2:
        changeworkspace(1);
        break;

      case KEY_3:
        changeworkspace(2);
        break;

      case KEY_4:
        changeworkspace(3);
        break;

      case KEY_5:
        changeworkspace(4);
        break;

      case KEY_6:
        changeworkspace(5);
        break;

      case KEY_7:
        changeworkspace(6);
        break;

      case KEY_8:
        changeworkspace(7);
        break;

      case KEY_9:
        changeworkspace(8);
        break;

      case KEY_0:
        changeworkspace(9);
        break;

      case KEY_Y:
        topleft();
        break;

      case KEY_U:
        topright();
        break;

      case KEY_B:
        botleft();
        break;

      case KEY_N:
        botright();
        break;

      case KEY_END:
        deletewin();
        break;

      case KEY_PREVSCR:
        prevscreen();
        break;

      case KEY_NEXTSCR:
        nextscreen();
        break;

      case KEY_ICONIFY:
        if (conf.allowicons) {
          hide(focuswin);
        }
        break;

      case KEY_PREVWS:
        if (curws > 0) {
          changeworkspace(curws - 1);
        } else {
          changeworkspace(WORKSPACES - 1);
        }
        break;

      case KEY_NEXTWS:
        changeworkspace((curws + 1) % WORKSPACES);
        break;

      default:
        /* Ignore other keys. */
        break;
    } /* switch unshifted */
  }
} /* handle_keypress() */

/* Helper function to configure a window. */
void
configwin(xcb_window_t win, uint16_t mask, WinConf wc) {
  uint32_t values[7];
  int i = -1;

  if (mask & XCB_CONFIG_WINDOW_X) {
    mask |= XCB_CONFIG_WINDOW_X;
    i++;
    values[i] = wc.x;
  }

  if (mask & XCB_CONFIG_WINDOW_Y) {
    mask |= XCB_CONFIG_WINDOW_Y;
    i++;
    values[i] = wc.y;
  }

  if (mask & XCB_CONFIG_WINDOW_WIDTH) {
    mask |= XCB_CONFIG_WINDOW_WIDTH;
    i++;
    values[i] = wc.width;
  }

  if (mask & XCB_CONFIG_WINDOW_HEIGHT) {
    mask |= XCB_CONFIG_WINDOW_HEIGHT;
    i++;
    values[i] = wc.height;
  }

  if (mask & XCB_CONFIG_WINDOW_SIBLING) {
    mask |= XCB_CONFIG_WINDOW_SIBLING;
    i++;
    values[i] = wc.sibling;
  }

  if (mask & XCB_CONFIG_WINDOW_STACK_MODE) {
    mask |= XCB_CONFIG_WINDOW_STACK_MODE;
    i++;
    values[i] = wc.stackmode;
  }

  if (-1 != i) {
    xcb_configure_window(conn, win, mask, values);
    xcb_flush(conn);
  }
}

void
configurerequest(xcb_configure_request_event_t *e) {
  Client *client;
  WinConf wc;
  int16_t mon_x;
  int16_t mon_y;
  uint16_t mon_width;
  uint16_t mon_height;

  log_verbose("event: configure request. mask = %d", e->value_mask);

  /* Find the client. */
  if ((client = findclient(e->window))) {
    /* Find monitor position and size. */
    if (NULL == client || NULL == client->monitor) {
      mon_x = 0;
      mon_y = 0;
      mon_width = screen->width_in_pixels;
      mon_height = screen->height_in_pixels;
    } else {
      mon_x = client->monitor->x;
      mon_y = client->monitor->y;
      mon_width = client->monitor->width;
      mon_height = client->monitor->height;
    }

#if 0
        /*
         * We ignore moves the user haven't initiated, that is do
         * nothing on XCB_CONFIG_WINDOW_X and XCB_CONFIG_WINDOW_Y
         * ConfigureRequests.
         *
         * Code here if we ever change our minds or if you, dear user,
         * wants this functionality.
         */

        if (e->value_mask & XCB_CONFIG_WINDOW_X)
        {
            /* Don't move window if maximized. Don't move off the screen. */
            if (!client->maxed && e->x > 0)
            {
                client->x = e->x;
            }
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_Y)
        {
            /*
             * Don't move window if maximized. Don't move off the
             * screen.
             */
            if (!client->maxed && !client->vertmaxed && e->y > 0)
            {
                client->y = e->y;
            }
        }
#endif

    if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
      /* Don't resize if maximized. */
      if (!client->maxed) {
        client->width = e->width;
      }
    }

    if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
      /* Don't resize if maximized. */
      if (!client->maxed && !client->vertmaxed) {
        client->height = e->height;
      }
    }

    /*
     * XXX Do we really need to pass on sibling and stack mode
     * configuration? Do we want to?
     */
    if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
      uint32_t values[1];

      values[0] = e->sibling;
      xcb_configure_window(conn, e->window, XCB_CONFIG_WINDOW_SIBLING, values);
      xcb_flush(conn);
    }

    if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
      uint32_t values[1];

      values[0] = e->stack_mode;
      xcb_configure_window(conn, e->window, XCB_CONFIG_WINDOW_STACK_MODE,
                           values);
      xcb_flush(conn);
    }

    /* Check if window fits on screen after resizing. */

    if (client->x + client->width + 2 * conf.borderwidth > mon_x + mon_width) {
      /*
       * See if it fits if we move away the window from the
       * right edge of the screen.
       */
      client->x = mon_x + mon_width - (client->width + 2 * conf.borderwidth);

      /*
       * If we moved over the left screen edge, move back and
       * fit exactly on screen.
       */
      if (client->x < mon_x) {
        client->x = mon_x;
        client->width = mon_width - 2 * conf.borderwidth;
      }
    }

    if (client->y + client->height + 2 * conf.borderwidth >
        mon_y + mon_height) {
      /*
       * See if it fits if we move away the window from the
       * bottom edge.
       */
      client->y = mon_y + mon_height - (client->height + 2 * conf.borderwidth);

      /*
       * If we moved over the top screen edge, move back and fit
       * on screen.
       */
      if (client->y < mon_y) {
        client->y = mon_y;
        client->height = mon_height - 2 * conf.borderwidth;
      }
    }

    moveresize(client->id, client->x, client->y, client->width, client->height);
  } else {

    /*
     * Unmapped window. Just pass all options except border
     * width.
     */
    wc.x = e->x;
    wc.y = e->y;
    wc.width = e->width;
    wc.height = e->height;
    wc.sibling = e->sibling;
    wc.stackmode = e->stack_mode;

    configwin(e->window, e->value_mask, wc);
  }
}

void
events(void) {
  xcb_generic_event_t *ev;

  int16_t mode_x = 0; /* X coord when in special mode */
  int16_t mode_y = 0; /* Y coord when in special mode */
  int x_fd;           /* Our X file descriptor */
  struct pollfd fds[2];

  /* Get the file descriptor so we can do select() on it. */
  x_fd = xcb_get_file_descriptor(conn);
  fds[0].fd = x_fd;
  fds[0].events = POLLIN;

  for (sigcode = 0; 0 == sigcode;) {
    /*
     * Check for events, again and again. When xcb_poll_for_event returns NULL
     * (and it does that a lot), we block on poll() until the
     * event file descriptor gets readable again.
     *
     * We do it this way instead of xcb_wait_for_event() since
     * poll() can poll for values using signal_fd. We
     * like that.
     */
    ev = xcb_poll_for_event(conn);
    if (NULL == ev) {
      /*
       * Check if we have an unrecoverable connection error,
       * like a disconnected X server.
       */
      if (xcb_connection_has_error(conn)) {
        cleanup(0);
        exit(1);
      }

      int found_poll = poll(fds, 1, -1);
      if (-1 == found_poll) {
        /* Something was seriously wrong with poll(). */
        log_err_final("poll failed");
        cleanup(0);
        exit(1);
      } else {
        /* We found more events. Goto start of loop. */
        continue;
      }
    }

    /* Note that we ignore XCB_RANDR_NOTIFY. */
    if (ev->response_type == randrbase + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
      log_verbose("RANDR screen change notify. checking outputs");
      getrandr();
      free(ev);
      continue;
    }

    switch (ev->response_type & ~0x80) {
      case XCB_MAP_REQUEST:
        {
          xcb_map_request_event_t *e;

          log_verbose("event: map request");
          e = (xcb_map_request_event_t *)ev;
          newwin(e->window);
        }
        break;

      case XCB_DESTROY_NOTIFY:
        {
          xcb_destroy_notify_event_t *e;

          e = (xcb_destroy_notify_event_t *)ev;

          /*
           * If we had focus or our last focus in this window,
           * forget about the focus.
           *
           * We will get an EnterNotify if there's another window
           * under the pointer so we can set the focus proper later.
           */
          if (NULL != focuswin) {
            if (focuswin->id == e->window) {
              focuswin = NULL;
            }
          }
          if (NULL != lastfocuswin) {
            if (lastfocuswin->id == e->window) {
              lastfocuswin = NULL;
            }
          }

          /*
           * Find this window in list of clients and forget about
           * it.
           */
          forgetwin(e->window);
        }
        break;

      case XCB_BUTTON_PRESS:
        {
          xcb_button_press_event_t *e;

          e = (xcb_button_press_event_t *)ev;
          log_verbose("button %d pressed in window %ld, subwindow %d "
                      "coordinates (%d,%d)",
                      e->detail, (long)e->event, e->child, e->event_x,
                      e->event_y);

          if (0 == e->child) {
            /* Mouse click on root window. Start programs? */

            switch (e->detail) {
              case 1: /* Mouse button one. */
                start(MOUSE1);
                break;

              case 2: /* Middle mouse button. */
                start(MOUSE2);
                break;

              case 3: /* Mouse button three. */
                start(MOUSE3);
                break;

              default:
                break;
            } /* switch */

            /* Break out of event switch. */
            break;
          }

          /*
           * If we don't have any currently focused window, we can't
           * do anything. We don't want to do anything if the mouse
           * cursor is in the wrong window (root window or a panel,
           * for instance). There is a limit to sloppy focus.
           */
          if (NULL == focuswin || focuswin->id != e->child) {
            break;
          }

          /*
           * If middle button was pressed, raise window or lower
           * it if it was already on top.
           */
          if (2 == e->detail) {
            raiseorlower(focuswin);
          } else {
            int16_t pointx;
            int16_t pointy;

            /* We're moving or resizing. */

            /*
             * Get and save pointer position inside the window
             * so we can go back to it when we're done moving
             * or resizing.
             */
            if (!getpointer(focuswin->id, &pointx, &pointy)) {
              break;
            }

            mode_x = pointx;
            mode_y = pointy;

            /* Raise window. */
            raisewindow(focuswin->id);

            /* Mouse button 1 was pressed. */
            if (1 == e->detail) {
              mode = MCWM_MOVE;

              /*
               * Warp pointer to upper left of window before
               * starting move.
               */
              xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, 1, 1);
            } else {
              /* Mouse button 3 was pressed. */

              mode = MCWM_RESIZE;

              /* Warp pointer to lower right. */
              xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                               focuswin->width, focuswin->height);
            }

            /*
             * Take control of the pointer in the root window
             * and confine it to root.
             *
             * Give us events when the key is released or if
             * any motion occurs with the key held down.
             *
             * Keep updating everything else.
             *
             * Don't use any new cursor.
             */
            xcb_grab_pointer(conn, 0, screen->root,
                             XCB_EVENT_MASK_BUTTON_RELEASE |
                                 XCB_EVENT_MASK_BUTTON_MOTION |
                                 XCB_EVENT_MASK_POINTER_MOTION_HINT,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             screen->root, XCB_NONE, XCB_CURRENT_TIME);

            xcb_flush(conn);
          }
        }
        break;

      case XCB_MOTION_NOTIFY:
        {
          xcb_query_pointer_reply_t *pointer;

          /*
           * We can't do anything if we don't have a focused window
           * or if it's fully maximized.
           */
          if (NULL == focuswin || focuswin->maxed) {
            break;
          }

          /*
           * This is not really a real notify, but just a hint that
           * the mouse pointer moved. This means we need to get the
           * current pointer position ourselves.
           */
          pointer = xcb_query_pointer_reply(
              conn, xcb_query_pointer(conn, screen->root), 0);

          if (NULL == pointer) {
            log_verbose("Couldn't get pointer position");
            break;
          }

          /*
           * Our pointer is moving and since we even get this event
           * we're either resizing or moving a window.
           */
          if (mode == MCWM_MOVE) {
            mousemove(focuswin, pointer->root_x, pointer->root_y);
          } else if (mode == MCWM_RESIZE) {
            mouseresize(focuswin, pointer->root_x, pointer->root_y);
          }
          free(pointer);
        }

        break;

      case XCB_BUTTON_RELEASE:

        if (0 == mode) {
          /*
           * Mouse button released, but not in a saved mode. Do
           * nothing.
           */
          break;
        } else {
          int16_t x;
          int16_t y;

          /* We're finished moving or resizing. */

          if (NULL == focuswin) {
            /*
             * We don't seem to have a focused window! Just
             * ungrab and reset the mode.
             */
            xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
            xcb_flush(conn); /* Important! */

            mode = 0;
            break;
          }

          /*
           * We will get an EnterNotify and focus another window
           * if the pointer just happens to be on top of another
           * window when we ungrab the pointer, so we have to
           * warp the pointer before to prevent this.
           *
           * Move to saved position within window or if that
           * position is now outside current window, move inside
           * window.
           */
          if (mode_x > focuswin->width) {
            x = focuswin->width / 2;
            if (0 == x) {
              x = 1;
            }

          } else {
            x = mode_x;
          }

          if (mode_y > focuswin->height) {
            y = focuswin->height / 2;
            if (0 == y) {
              y = 1;
            }
          } else {
            y = mode_y;
          }

          xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0, x, y);
          xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
          xcb_flush(conn); /* Important! */

          mode = 0;
        }
        break;

      case XCB_KEY_PRESS:
        {
          xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;

          handle_keypress(e);
        }
        break;

      case XCB_KEY_RELEASE:
        {
          xcb_key_release_event_t *e = (xcb_key_release_event_t *)ev;
          unsigned i;

          if (MCWM_TABBING == mode) {
            /*
             * Check if it's the that was released was a key
             * generating the MODKEY mask.
             */
            for (i = 0; i < modkeys.len; i++) {

              if (e->detail == modkeys.keycodes[i]) {
                finishtabbing();

                /* Get out of for... */
                break;
              }
            }
          }
        }
        break;

      case XCB_ENTER_NOTIFY:
        {
          xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;
          Client *client;

          /*
           * If this isn't a normal enter notify, don't bother.
           *
           * We also need ungrab events, since these will be
           * generated on button and key grabs and if the user for
           * some reason presses a button on the root and then moves
           * the pointer to our window and releases the button, we
           * get an Ungrab EnterNotify.
           *
           * The other cases means the pointer is grabbed and that
           * either means someone is using it for menu selections or
           * that we're moving or resizing. We don't want to change
           * focus in those cases.
           */
          if (e->mode == XCB_NOTIFY_MODE_NORMAL ||
              e->mode == XCB_NOTIFY_MODE_UNGRAB) {
            /*
             * If we're entering the same window we focus now,
             * then don't bother focusing.
             */
            if (NULL == focuswin || e->event != focuswin->id) {
              /*
               * Otherwise, set focus to the window we just
               * entered if we can find it among the windows we
               * know about. If not, just keep focus in the old
               * window.
               */
              client = findclient(e->event);
              if (NULL != client) {
                if (MCWM_TABBING != mode) {
                  /*
                   * We are focusing on a new window. Since
                   * we're not currently tabbing around the
                   * window ring, we need to update the
                   * current workspace window list: Move
                   * first the old focus to the head of the
                   * list and then the new focus to the head
                   * of the list.
                   */
                  if (NULL != focuswin) {
                    list_move_head(&wslist[curws], focuswin->wsitem[curws]);
                    lastfocuswin = NULL;
                  }

                  list_move_head(&wslist[curws], client->wsitem[curws]);
                }

                setfocus(client);
              }
            }
          }
        }
        break;

      case XCB_CONFIGURE_NOTIFY:
        {
          xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;

          if (e->window == screen->root) {
            /*
             * When using RANDR or Xinerama, the root can change
             * geometry when the user adds a new screen, tilts
             * their screen 90 degrees or whatnot. We might need
             * to rearrange windows to be visible.
             *
             * We might get notified for several reasons, not just
             * if the geometry changed. If the geometry is
             * unchanged we do nothing.
             */

            if (e->width == screen->width_in_pixels &&
                e->height == screen->height_in_pixels) {
              /* Root geometry is really unchanged. Do nothing. */
            } else {
              screen->width_in_pixels = e->width;
              screen->height_in_pixels = e->height;

              /* Check for RANDR. */
              if (-1 == randrbase) {
                /* We have no RANDR so we rearrange windows to
                 * the new root geometry here.
                 *
                 * With RANDR enabled, we handle this per
                 * screen getrandr() when we receive an
                 * XCB_RANDR_SCREEN_CHANGE_NOTIFY event.
                 */
                arrangewindows();
              }
            }
          }
        }
        break;

      case XCB_CONFIGURE_REQUEST:
        configurerequest((xcb_configure_request_event_t *)ev);
        break;

      case XCB_CLIENT_MESSAGE:
        {
          xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;

          if (conf.allowicons) {
            if (e->type == wm_change_state && e->format == 32 &&
                e->data.data32[0] == XCB_ICCCM_WM_STATE_ICONIC) {
              long data[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};

              /* Unmap window and declare iconic. */

              xcb_unmap_window(conn, e->window);
              xcb_change_property(conn, XCB_PROP_MODE_REPLACE, e->window,
                                  wm_state, wm_state, 32, 2, data);
              xcb_flush(conn);
            }
          }
        }
        break;

      case XCB_CIRCULATE_REQUEST:
        {
          xcb_circulate_request_event_t *e =
              (xcb_circulate_request_event_t *)ev;

          /*
           * Subwindow e->window to parent e->event is about to be
           * restacked.
           *
           * Just do what was requested, e->place is either
           * XCB_PLACE_ON_TOP or _ON_BOTTOM. We don't care.
           */
          xcb_circulate_window(conn, e->window, e->place);
        }
        break;

      case XCB_MAPPING_NOTIFY:
        {
          xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *)ev;

          /*
           * XXX Gah! We get a new notify message for *every* key!
           * We want to know when the entire keyboard is finished.
           * Impossible? Better handling somehow?
           */

          /*
           * We're only interested in keys and modifiers, not
           * pointer mappings, for instance.
           */
          if (e->request != XCB_MAPPING_MODIFIER &&
              e->request != XCB_MAPPING_KEYBOARD) {
            break;
          }

          /* Forget old key bindings. */
          xcb_ungrab_key(conn, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);

          /* Use the new ones. */
          setupkeys();
        }
        break;

      case XCB_UNMAP_NOTIFY:
        {
          xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *)ev;
          LinkedNode *item;
          Client *client;

          /*
           * Find the window in our *current* workspace list, then
           * forget about it. If it gets mapped, we add it to our
           * lists again then.
           *
           * Note that we might not know about the window we got the
           * UnmapNotify event for. It might be a window we just
           * unmapped on *another* workspace when changing
           * workspaces, for instance, or it might be a window with
           * override redirect set. This is not an error.
           *
           * XXX We might need to look in the global window list,
           * after all. Consider if a window is unmapped on our last
           * workspace while changing workspaces... If we do this,
           * we need to keep track of our own windows and ignore
           * UnmapNotify on them.
           */
          for (item = wslist[curws]; item != NULL; item = item->next) {
            client = item->data;

            if (client->id == e->window) {
              if (focuswin == client) {
                focuswin = NULL;
              }

              forgetclient(client);
              /* We're finished. Break out of for loop. */
              break;
            }
          }
        }
        break;
    }

    /* Forget about this event. */
    free(ev);
  }
}

void
printhelp(void) {
  printf("mcwm: Usage: mcwm [-b] [-s snapmargin] [-t terminal-program] "
         "[-f colour] [-u colour] [-x colour] \n");
  printf("  -b means draw no borders\n");
  printf("  -s snapmargin in pixels\n");
  printf("  -t urxvt will start urxvt when MODKEY + Return is pressed\n");
  printf("  -f colour sets colour for focused window borders of focused "
         "to a named color.\n");
  printf("  -u colour sets colour for unfocused window borders.\n");
  printf("  -x color sets colour for fixed window borders.\n");
}

void
sigcatch(int sig) {
  sigcode = sig;
}

/*
 * Get a defined atom from the X server.
 */
xcb_atom_t
getatom(char *atom_name) {
  xcb_intern_atom_cookie_t atom_cookie;
  xcb_atom_t atom;
  xcb_intern_atom_reply_t *rep;

  atom_cookie = xcb_intern_atom(conn, 0, strlen(atom_name), atom_name);
  rep = xcb_intern_atom_reply(conn, atom_cookie, NULL);
  if (NULL != rep) {
    atom = rep->atom;
    free(rep);
    return atom;
  }

  /*
   * XXX Note that we return 0 as an atom if anything goes wrong.
   * Might become interesting.
   */
  return 0;
}

int
main(int argc, char **argv) {
  uint32_t mask = 0;
  uint32_t values[2];
  int ch; /* Option character */
  xcb_void_cookie_t cookie;
  xcb_generic_error_t *error;
  xcb_drawable_t root;
  char *focuscol;
  char *unfocuscol;
  char *fixedcol;
  int scrno;
  xcb_screen_iterator_t iter;

  /* Install signal handlers. */

  /* We ignore child exists. Don't create zombies. */
  if (SIG_ERR == signal(SIGCHLD, SIG_IGN)) {
    perror("mcwm: signal");
    exit(1);
  }

  if (SIG_ERR == signal(SIGINT, sigcatch)) {
    perror("mcwm: signal");
    exit(1);
  }

  if (SIG_ERR == signal(SIGTERM, sigcatch)) {
    perror("mcwm: signal");
    exit(1);
  }

  /* Set up defaults. */

  conf.borderwidth = BORDERWIDTH;
  conf.snapmargin = SNAPMARGIN;
  conf.terminal = TERMINAL;
  conf.allowicons = ALLOWICONS;
  focuscol = FOCUSCOL;
  unfocuscol = UNFOCUSCOL;
  fixedcol = FIXEDCOL;

  opterr = 0;
  while (1) {
    ch = getopt(argc, argv, "b:s:it:f:u:x:v");
    if (-1 == ch) {
      /* No more options, break out of while loop. */
      break;
    }

    switch (ch) {
      case 'b':
        /* Border width */
        conf.borderwidth = atoi(optarg);
        break;

      case 's':
        /* Snap margin */
        conf.snapmargin = atoi(optarg);
        break;

      case 'i':
        conf.allowicons = true;
        break;

      case 't':
        conf.terminal = optarg;
        break;

      case 'f':
        focuscol = optarg;
        break;

      case 'u':
        unfocuscol = optarg;
        break;

      case 'x':
        fixedcol = optarg;
        break;

      case 'v':
        verbose = 1;
        break;

      case '?':
        if (optopt == 'b' || optopt == 's' || optopt == 'i' || optopt == 't' ||
            optopt == 'f' || optopt == 'u' || optopt == 'x' || optopt == 'v') {
          log_err_final("expected argument after -%c", optopt);
        } else {
          log_err_final("invalid argument -%c", optopt);
        }
      default:
        printhelp();
        exit(0);
    } /* switch */
  }

  /*
   * Use $DISPLAY. After connecting scrno will contain the value of
   * the display's screen.
   */
  conn = xcb_connect(NULL, &scrno);
  if (xcb_connection_has_error(conn)) {
    perror("xcb_connect");
    exit(1);
  }

  /* Find our screen. */
  iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
  for (int i = 0; i < scrno; ++i) {
    xcb_screen_next(&iter);
  }

  screen = iter.data;
  if (!screen) {
    log_err_final("can't get the current screen");
    xcb_disconnect(conn);
    exit(1);
  }

  root = screen->root;

  /* Get some colours. */
  conf.focuscol = getcolor(focuscol);
  conf.unfocuscol = getcolor(unfocuscol);
  conf.fixedcol = getcolor(fixedcol);

  /* Get some atoms. */
  atom_desktop = getatom("_NET_WM_DESKTOP");
  wm_delete_window = getatom("WM_DELETE_WINDOW");
  wm_change_state = getatom("WM_CHANGE_STATE");
  wm_state = getatom("WM_STATE");
  wm_protocols = getatom("WM_PROTOCOLS");

  /* Check for RANDR extension and configure. */
  randrbase = setuprandr();

  /* Loop over all clients and set up stuff. */
  if (0 != setupscreen()) {
    log_err_final("failed to initialize windows");
    xcb_disconnect(conn);
    exit(1);
  }

  /* Set up key bindings. */
  if (0 != setupkeys()) {
    log_err_final("couldn't set up keycodes");
    xcb_disconnect(conn);
    exit(1);
  }

  /* Grab mouse buttons. */

  xcb_grab_button(conn, 0, root,
                  XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                  1 /* left mouse button */, MOUSEMODKEY);

  xcb_grab_button(conn, 0, root,
                  XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                  2 /* middle mouse button */, MOUSEMODKEY);

  xcb_grab_button(conn, 0, root,
                  XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                  3 /* right mouse button */, MOUSEMODKEY);

  /* Subscribe to events. */
  mask = XCB_CW_EVENT_MASK;

  values[0] = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
              XCB_EVENT_MASK_STRUCTURE_NOTIFY |
              XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

  cookie = xcb_change_window_attributes_checked(conn, root, mask, values);
  error = xcb_request_check(conn, cookie);

  xcb_flush(conn);

  if (NULL != error) {
    log_err_final("failed to initialize window manager, err: %d",
                  error->error_code);

    xcb_disconnect(conn);

    exit(1);
  }

  /* Loop over events. */
  events();

  /* Die gracefully. */
  cleanup(sigcode);

  exit(0);
}
