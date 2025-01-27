#ifndef EVENTS_H
#define EVENTS_H

#define MAXEVENTS 34

static char *evnames[] = {"",
                          "",
                          "KeyPress",
                          "KeyRelease",
                          "ButtonPress",
                          "ButtonRelease",
                          "MotionNotify",
                          "EnterNotify",
                          "LeaveNotify",
                          "FocusIn",
                          "FocusOut",
                          "KeymapNotify",
                          "Expose",
                          "GraphicsExpose",
                          "NoExpose",
                          "VisibilityNotify",
                          "CreateNotify",
                          "DestroyNotify",
                          "UnmapNotify",
                          "MapNotify",
                          "MapRequest",
                          "ReparentNotify",
                          "ConfigureNotify",
                          "ConfigureRequest",
                          "GravityNotify",
                          "ResizeRequest",
                          "CirculateNotify",
                          "CirculateRequest",
                          "PropertyNotify",
                          "SelectionClear",
                          "SelectionRequest",
                          "SelectionNotify",
                          "ColormapNotify",
                          "ClientMessage",
                          "MappingNotify"};

#endif
