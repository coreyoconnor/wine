/*
 * X events handling functions
 * 
 * Copyright 1993 Alexandre Julliard
 * 
 */

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include "windows.h"
#include "gdi.h"
#include "heap.h"
#include "win.h"
#include "class.h"
#include "clipboard.h"
#include "debugger.h"
#include "hook.h"
#include "message.h"
#include "module.h"
#include "options.h"
#include "queue.h"
#include "winpos.h"
#include "registers.h"
#include "stddebug.h"
#include "debug.h"
#include "dde_proc.h"

#define NB_BUTTONS      3     /* Windows can handle 3 buttons */

  /* X context to associate a hwnd to an X window */
static XContext winContext = 0;

  /* State variables */
BOOL MouseButtonsStates[NB_BUTTONS];
BOOL AsyncMouseButtonsStates[NB_BUTTONS];
BYTE KeyStateTable[256];
BYTE AsyncKeyStateTable[256];


       WPARAM   lastEventChar = 0; /* this will have to be changed once
				    * ToAscii starts working */

static HWND 	captureWnd = 0;
static BOOL	InputEnabled = TRUE;

/* Keyboard translation tables */
static const int special_key[] =
{
    VK_BACK, VK_TAB, 0, VK_CLEAR, 0, VK_RETURN, 0, 0,           /* FF08 */
    0, 0, 0, VK_PAUSE, VK_SCROLL, 0, 0, 0,                      /* FF10 */
    0, 0, 0, VK_ESCAPE                                          /* FF18 */
};

static const int cursor_key[] =
{
    VK_HOME, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_PRIOR, 
                                       VK_NEXT, VK_END          /* FF50 */
};

static const int misc_key[] =
{
    VK_SELECT, VK_SNAPSHOT, VK_EXECUTE, VK_INSERT, 0, 0, 0, 0,  /* FF60 */
    VK_CANCEL, VK_HELP, VK_CANCEL, VK_MENU                      /* FF68 */
};

static const int keypad_key[] =
{
    VK_MENU, VK_NUMLOCK,                                        /* FF7E */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* FF80 */
    0, 0, 0, 0, 0, VK_RETURN, 0, 0,                             /* FF88 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* FF90 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* FF98 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* FFA0 */
    0, 0, VK_MULTIPLY, VK_ADD, VK_SEPARATOR, VK_SUBTRACT, 
                               VK_DECIMAL, VK_DIVIDE,           /* FFA8 */
    VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
                            VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, /* FFB0 */
    VK_NUMPAD8, VK_NUMPAD9                                      /* FFB8 */
};
    
static const int function_key[] =
{
    VK_F1, VK_F2,                                               /* FFBE */
    VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10,    /* FFC0 */
    VK_F11, VK_F12, VK_F13, VK_F14, VK_F15, VK_F16              /* FFC8 */
};

static const int modifier_key[] =
{
    VK_SHIFT, VK_SHIFT, VK_CONTROL, VK_CONTROL, VK_CAPITAL,
                                                0, 0,           /* FFE1 */
    0, VK_MENU, VK_MENU                                         /* FFE8 */
};

typedef union
{
    struct
    {
	unsigned long count : 16;
	unsigned long code : 8;
	unsigned long extended : 1;
	unsigned long : 2;
	unsigned long reserved : 2;
	unsigned long context : 1;
	unsigned long previous : 1;
	unsigned long transition : 1;
    } lp1;
    unsigned long lp2;
} KEYLP;

static BOOL KeyDown = FALSE;

static const char * const event_names[] =
{
    "", "", "KeyPress", "KeyRelease", "ButtonPress", "ButtonRelease",
    "MotionNotify", "EnterNotify", "LeaveNotify", "FocusIn", "FocusOut",
    "KeymapNotify", "Expose", "GraphicsExpose", "NoExpose", "VisibilityNotify",
    "CreateNotify", "DestroyNotify", "UnmapNotify", "MapNotify", "MapRequest",
    "ReparentNotify", "ConfigureNotify", "ConfigureRequest", "GravityNotify",
    "ResizeRequest", "CirculateNotify", "CirculateRequest", "PropertyNotify",
    "SelectionClear", "SelectionRequest", "SelectionNotify", "ColormapNotify",
    "ClientMessage", "MappingNotify"
};

  /* Event handlers */
static void EVENT_key( XKeyEvent *event );
static void EVENT_ButtonPress( XButtonEvent *event );
static void EVENT_ButtonRelease( XButtonEvent *event );
static void EVENT_MotionNotify( XMotionEvent *event );
static void EVENT_FocusIn( HWND hwnd, XFocusChangeEvent *event );
static void EVENT_FocusOut( HWND hwnd, XFocusChangeEvent *event );
static void EVENT_Expose( WND *pWnd, XExposeEvent *event );
static void EVENT_GraphicsExpose( WND *pWnd, XGraphicsExposeEvent *event );
static void EVENT_ConfigureNotify( HWND hwnd, XConfigureEvent *event );
static void EVENT_SelectionRequest( WND *pWnd, XSelectionRequestEvent *event);
static void EVENT_SelectionNotify( XSelectionEvent *event);
static void EVENT_SelectionClear( WND *pWnd, XSelectionClearEvent *event);
static void EVENT_ClientMessage( WND *pWnd, XClientMessageEvent *event );
static void EVENT_MapNotify( HWND hwnd, XMapEvent *event );

/* Usable only with OLVWM - compile option perhaps?
static void EVENT_EnterNotify( WND *pWnd, XCrossingEvent *event );
*/

/***********************************************************************
 *           EVENT_ProcessEvent
 *
 * Process an X event.
 */
void EVENT_ProcessEvent( XEvent *event )
{
    WND *pWnd;
    
    if (XFindContext( display, ((XAnyEvent *)event)->window, winContext,
                      (char **)&pWnd ) != 0)
        return;  /* Not for a registered window */

    dprintf_event( stddeb, "Got event %s for hwnd %04x\n",
                   event_names[event->type], pWnd->hwndSelf );

    switch(event->type)
    {
    case KeyPress:
    case KeyRelease:
        if (!HOOK_GetHook(WH_JOURNALPLAYBACK, 0) )
            EVENT_key( (XKeyEvent*)event );
	break;
	
    case ButtonPress:
        if (!HOOK_GetHook(WH_JOURNALPLAYBACK, 0) )
            EVENT_ButtonPress( (XButtonEvent*)event );
	break;

    case ButtonRelease:
        if (!HOOK_GetHook(WH_JOURNALPLAYBACK, 0) )
            EVENT_ButtonRelease( (XButtonEvent*)event );
	break;

    case MotionNotify:
        /* Wine between two fast machines across the overloaded campus
	   ethernet gets very boged down in MotionEvents. The following
	   simply finds the last motion event in the queue and drops
	   the rest. On a good link events are servered before they build
	   up so this doesn't take place. On a slow link this may cause
	   problems if the event order is important. I'm not yet seen
	   of any problems. Jon 7/6/96.
	 */
	if (!HOOK_GetHook(WH_JOURNALPLAYBACK, 0) )
	{
            while (XCheckTypedWindowEvent(display,((XAnyEvent *)event)->window,
                                          MotionNotify, event));    
            EVENT_MotionNotify( (XMotionEvent*)event );
	}
	break;

    case FocusIn:
        EVENT_FocusIn( pWnd->hwndSelf, (XFocusChangeEvent*)event );
	break;

    case FocusOut:
	EVENT_FocusOut( pWnd->hwndSelf, (XFocusChangeEvent*)event );
	break;

    case Expose:
	EVENT_Expose( pWnd, (XExposeEvent *)event );
	break;

    case GraphicsExpose:
	EVENT_GraphicsExpose( pWnd, (XGraphicsExposeEvent *)event );
        break;

    case ConfigureNotify:
	EVENT_ConfigureNotify( pWnd->hwndSelf, (XConfigureEvent*)event );
	break;

    case SelectionRequest:
	EVENT_SelectionRequest( pWnd, (XSelectionRequestEvent *)event );
	break;

    case SelectionNotify:
	EVENT_SelectionNotify( (XSelectionEvent *)event );
	break;

    case SelectionClear:
	EVENT_SelectionClear( pWnd, (XSelectionClearEvent*) event );
	break;

    case ClientMessage:
	EVENT_ClientMessage( pWnd, (XClientMessageEvent *) event );
	break;
/*  case EnterNotify:
 *       EVENT_EnterNotify( pWnd, (XCrossingEvent *) event );
 *       break;
 */
    case NoExpose:
	break;

    /* We get all these because of StructureNotifyMask. */
    case UnmapNotify:
    case CirculateNotify:
    case CreateNotify:
    case DestroyNotify:
    case GravityNotify:
    case ReparentNotify:
	break;

    case MapNotify:
	EVENT_MapNotify( pWnd->hwndSelf, (XMapEvent *)event );
	break;

    default:    
	dprintf_event(stddeb, "Unprocessed event %s for hwnd %04x\n",
	        event_names[event->type], pWnd->hwndSelf );
	break;
    }
}


/***********************************************************************
 *           EVENT_RegisterWindow
 *
 * Associate an X window to a HWND.
 */
void EVENT_RegisterWindow( WND *pWnd )
{
    if (!winContext) winContext = XUniqueContext();
    XSaveContext( display, pWnd->window, winContext, (char *)pWnd );
}


/***********************************************************************
 *           EVENT_WaitXEvent
 *
 * Wait for an X event, optionally sleeping until one arrives.
 * Return TRUE if an event is pending, FALSE on timeout or error
 * (for instance lost connection with the server).
 */
BOOL32 EVENT_WaitXEvent( BOOL32 sleep )
{
    fd_set read_set;
    struct timeval timeout;
    XEvent event;
    int fd = ConnectionNumber(display);

    if (!XPending(display))
    {
        LONG maxWait = sleep ? TIMER_GetNextExpiration() : 0;

        FD_ZERO( &read_set );
        FD_SET( fd, &read_set );

	timeout.tv_usec = (maxWait % 1000) * 1000;
	timeout.tv_sec = maxWait / 1000;

#ifdef CONFIG_IPC
	sigsetjmp(env_wait_x, 1);
	stop_wait_op= CONT;
	    
	if (DDE_GetRemoteMessage()) {
	    while(DDE_GetRemoteMessage())
		;
	    return TRUE;
	}
	stop_wait_op = STOP_WAIT_X;
	/* The code up to the next "stop_wait_op = CONT" must be reentrant */
	if (select( fd+1, &read_set, NULL, NULL, &timeout ) != 1 &&
	    !XPending(display))
        {
	    stop_wait_op = CONT;
            TIMER_ExpireTimers();
	    return FALSE;
	}
        else stop_wait_op = CONT;
#else  /* CONFIG_IPC */
	if (select( fd+1, &read_set, NULL, NULL, &timeout ) != 1)
        {
            /* Timeout or error */
            TIMER_ExpireTimers();
            return FALSE;
        }
#endif  /* CONFIG_IPC */

    }

    /* Process the event (and possibly others that occurred in the meantime) */
    do
    {

#ifdef CONFIG_IPC
        if (DDE_GetRemoteMessage())
        {
            while(DDE_GetRemoteMessage()) ;
            return TRUE;
        }
#endif  /* CONFIG_IPC */

        XNextEvent( display, &event );
        EVENT_ProcessEvent( &event );
    }
    while (XPending( display ));
    return TRUE;
}


/***********************************************************************
 *           EVENT_Synchronize
 *
 * Synchronize with the X server. Should not be used too often.
 */
void EVENT_Synchronize()
{
    XEvent event;

    XSync( display, False );
    while (XPending( display ))
    {
	XNextEvent( display, &event );
	EVENT_ProcessEvent( &event );
    }    
}


/***********************************************************************
 *           EVENT_XStateToKeyState
 *
 * Translate a X event state (Button1Mask, ShiftMask, etc...) to
 * a Windows key state (MK_SHIFT, MK_CONTROL, etc...)
 */
static WORD EVENT_XStateToKeyState( int state )
{
    int kstate = 0;
    
    if (state & Button1Mask) kstate |= MK_LBUTTON;
    if (state & Button2Mask) kstate |= MK_MBUTTON;
    if (state & Button3Mask) kstate |= MK_RBUTTON;
    if (state & ShiftMask)   kstate |= MK_SHIFT;
    if (state & ControlMask) kstate |= MK_CONTROL;
    return kstate;
}


/***********************************************************************
 *           EVENT_Expose
 */
static void EVENT_Expose( WND *pWnd, XExposeEvent *event )
{
    RECT32 rect;

    /* Make position relative to client area instead of window */
    rect.left   = event->x - (pWnd->rectClient.left - pWnd->rectWindow.left);
    rect.top    = event->y - (pWnd->rectClient.top - pWnd->rectWindow.top);
    rect.right  = rect.left + event->width;
    rect.bottom = rect.top + event->height;

    PAINT_RedrawWindow( pWnd->hwndSelf, &rect, 0,
                        RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_ERASE |
                        (event->count ? 0 : RDW_ERASENOW), 0 );
}


/***********************************************************************
 *           EVENT_GraphicsExpose
 *
 * This is needed when scrolling area is partially obscured
 * by non-Wine X window.
 */
static void EVENT_GraphicsExpose( WND *pWnd, XGraphicsExposeEvent *event )
{
    RECT32 rect;

    /* Make position relative to client area instead of window */
    rect.left   = event->x - (pWnd->rectClient.left - pWnd->rectWindow.left);
    rect.top    = event->y - (pWnd->rectClient.top - pWnd->rectWindow.top);
    rect.right  = rect.left + event->width;
    rect.bottom = rect.top + event->height;

    PAINT_RedrawWindow( pWnd->hwndSelf, &rect, 0,
                        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE |
                        (event->count ? 0 : RDW_ERASENOW), 0 );
}


/***********************************************************************
 *           EVENT_key
 *
 * Handle a X key event
 */
static void EVENT_key( XKeyEvent *event )
{
    char Str[24]; 
    XComposeStatus cs; 
    KeySym keysym;
    WORD vkey = 0;
    WORD xkey, key_type, key;
    KEYLP keylp;
    BOOL extended = FALSE;

    int ascii_chars = XLookupString(event, Str, 1, &keysym, &cs);

    Str[ascii_chars] = '\0';
    dprintf_key(stddeb,"WM_KEY??? : keysym=%lX, ascii chars=%u / %X / '%s'\n", 
	   keysym, ascii_chars, Str[0], Str);

    /* Ctrl-Alt-Return enters the debugger */
    if ((keysym == XK_Return) && (event->type == KeyPress) &&
        (event->state & ControlMask) && (event->state & Mod1Mask))
        DEBUG_EnterDebugger();

    xkey = LOWORD(keysym);
    key_type = HIBYTE(xkey);
    key = LOBYTE(xkey);
    dprintf_key(stddeb,"            key_type=%X, key=%X\n", key_type, key);

    if (key_type == 0xFF)                          /* non-character key */
    {
	if (key >= 0x08 && key <= 0x1B)            /* special key */
	    vkey = special_key[key - 0x08];
	else if (key >= 0x50 && key <= 0x57)       /* cursor key */
	    vkey = cursor_key[key - 0x50];
	else if (key >= 0x60 && key <= 0x6B)       /* miscellaneous key */
	    vkey = misc_key[key - 0x60];
	else if (key >= 0x7E && key <= 0xB9)       /* keypad key */
	{
	    vkey = keypad_key[key - 0x7E];
	    extended = TRUE;
	}
	else if (key >= 0xBE && key <= 0xCD)       /* function key */
	{
	    vkey = function_key[key - 0xBE];
	    extended = TRUE;
	}
	else if (key >= 0xE1 && key <= 0xEA)       /* modifier key */
	    vkey = modifier_key[key - 0xE1];
	else if (key == 0xFF)                      /* DEL key */
	    vkey = VK_DELETE;
    }
    else if (key_type == 0)                        /* character key */
    {
	if ( isalnum(key) )
	     vkey = toupper(key);                  /* convert lc to uc */
	else if ( isspace(key) )
	  vkey = key;				   /* XXX approximately */
        else  
	  switch (key)				   /* the rest... */
	  {
#define vkcase(k,val) case k: vkey = val; break;
#define vkcase2(k1,k2,val) case k1: case k2: vkey = val; break;

	      /* I wish I had a bit-paired keyboard! */
	      vkcase('!','1'); vkcase('@','2'); vkcase('#','3');
	      vkcase('$','4'); vkcase('%','5'); vkcase('^','6');
	      vkcase('&','7'); vkcase('*','8'); vkcase('(','9');
	      vkcase(')','0');

	      vkcase2('`','~',0xc0);
	      vkcase2('-','_',0xbd);
	      vkcase2('=','+',0xbb);
	      vkcase2('[','{',0xdb);
	      vkcase2(']','}',0xdd);
	      vkcase2(';',':',0xba);
	      vkcase2('\'','\"',0xde);
	      vkcase2(',','<',0xbc);
	      vkcase2('.','>',0xbe);
	      vkcase2('/','?',0xbf);
	      vkcase2('\\','|',0xdc);
#undef vkcase
#undef vkcase2
	    default:
	      fprintf( stderr, "Unknown key! Please report!\n" );
	      vkey = 0;				   /* whatever */
	  }
    }

    if (event->type == KeyPress)
    {
        if (!(KeyStateTable[vkey] & 0x80))
            KeyStateTable[vkey] ^= 0x01;
	KeyStateTable[vkey] |= 0x80;
	keylp.lp1.count = 1;
	keylp.lp1.code = LOBYTE(event->keycode) - 8;
	keylp.lp1.extended = (extended ? 1 : 0);
	keylp.lp1.reserved = (ascii_chars ? 1 : 0);
	keylp.lp1.context = ( (event->state & Mod1Mask) || 
			       (KeyStateTable[VK_MENU] & 0x80)) ? 1 : 0;
	keylp.lp1.previous = (KeyDown ? 0 : 1);
	keylp.lp1.transition = 0;
	dprintf_key(stddeb,"            wParam=%X, lParam=%lX\n", 
		    vkey, keylp.lp2 );
	dprintf_key(stddeb,"            KeyState=%X\n", KeyStateTable[vkey]);
	hardware_event( KeyStateTable[VK_MENU] & 0x80 ? WM_SYSKEYDOWN : WM_KEYDOWN, 
		        vkey, keylp.lp2,
		        event->x_root - desktopX, event->y_root - desktopY,
		        event->time - MSG_WineStartTicks, 0 );
	KeyDown = TRUE;

	/* Currently we use reserved field in the scan-code byte to
	 * make it possible for TranslateMessage to recognize character keys
	 * and get them from lastEventChar global variable.
	 *
	 * ToAscii should handle it.
	 */

	if( ascii_chars ) lastEventChar = Str[0];
    }
    else
    {
	UINT sysKey = KeyStateTable[VK_MENU];

	KeyStateTable[vkey] &= ~0x80; 
	keylp.lp1.count = 1;
	keylp.lp1.code = LOBYTE(event->keycode) - 8;
	keylp.lp1.extended = (extended ? 1 : 0);
	keylp.lp1.reserved = 0;
	keylp.lp1.context = (event->state & Mod1Mask ? 1 : 0);
	keylp.lp1.previous = 1;
	keylp.lp1.transition = 1;
	dprintf_key(stddeb,"            wParam=%X, lParam=%lX\n", 
		    vkey, keylp.lp2 );
	dprintf_key(stddeb,"            KeyState=%X\n", KeyStateTable[vkey]);
	hardware_event( sysKey & 0x80 ? WM_SYSKEYUP : WM_KEYUP, 
		        vkey, keylp.lp2,
		        event->x_root - desktopX, event->y_root - desktopY,
		        event->time - MSG_WineStartTicks, 0 );
	KeyDown = FALSE;
    }
}


/***********************************************************************
 *           EVENT_MotionNotify
 */
static void EVENT_MotionNotify( XMotionEvent *event )
{
    hardware_event( WM_MOUSEMOVE, EVENT_XStateToKeyState( event->state ), 0L,
		    event->x_root - desktopX, event->y_root - desktopY,
		    event->time - MSG_WineStartTicks, 0 );
}


/***********************************************************************
 *           EVENT_DummyMotionNotify
 *
 * Generate a dummy MotionNotify event. Used to force a WM_SETCURSOR message.
 */
void EVENT_DummyMotionNotify(void)
{
    Window root, child;
    int rootX, rootY, childX, childY;
    unsigned int state;

    if (XQueryPointer( display, rootWindow, &root, &child,
                       &rootX, &rootY, &childX, &childY, &state ))
    {
        hardware_event(WM_MOUSEMOVE, EVENT_XStateToKeyState( state ), 0L,
                       rootX - desktopX, rootY - desktopY, GetTickCount(), 0 );
    }
}


/***********************************************************************
 *           EVENT_ButtonPress
 */
static void EVENT_ButtonPress( XButtonEvent *event )
{
    static WORD messages[NB_BUTTONS] = 
        { WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN };
    int buttonNum = event->button - 1;

    if (buttonNum >= NB_BUTTONS) return;
    MouseButtonsStates[buttonNum] = 0x8000;
    AsyncMouseButtonsStates[buttonNum] = 0x8000;
    hardware_event( messages[buttonNum],
		    EVENT_XStateToKeyState( event->state ), 0L,
		    event->x_root - desktopX, event->y_root - desktopY,
		    event->time - MSG_WineStartTicks, 0 );
}


/***********************************************************************
 *           EVENT_ButtonRelease
 */
static void EVENT_ButtonRelease( XButtonEvent *event )
{
    static const WORD messages[NB_BUTTONS] = 
        { WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP };
    int buttonNum = event->button - 1;

    if (buttonNum >= NB_BUTTONS) return;    
    MouseButtonsStates[buttonNum] = FALSE;
    hardware_event( messages[buttonNum],
		    EVENT_XStateToKeyState( event->state ), 0L,
		    event->x_root - desktopX, event->y_root - desktopY,
		    event->time - MSG_WineStartTicks, 0 );
}


/**********************************************************************
 *              EVENT_FocusIn
 */
static void EVENT_FocusIn (HWND hwnd, XFocusChangeEvent *event )
{
    if (event->detail == NotifyPointer) return;
    if (hwnd != GetActiveWindow()) WINPOS_ChangeActiveWindow( hwnd, FALSE );
    if ((hwnd != GetFocus32()) && !IsChild( hwnd, GetFocus32()))
        SetFocus32( hwnd );
}


/**********************************************************************
 *              EVENT_FocusOut
 *
 * Note: only top-level override-redirect windows get FocusOut events.
 */
static void EVENT_FocusOut( HWND hwnd, XFocusChangeEvent *event )
{
    if (event->detail == NotifyPointer) return;
    if (hwnd == GetActiveWindow()) WINPOS_ChangeActiveWindow( 0, FALSE );
    if ((hwnd == GetFocus32()) || IsChild( hwnd, GetFocus32()))
        SetFocus32( 0 );
}


/**********************************************************************
 *              EVENT_ConfigureNotify
 *
 * The ConfigureNotify event is only selected on the desktop window
 * and on top-level windows when the -managed flag is used.
 */
static void EVENT_ConfigureNotify( HWND hwnd, XConfigureEvent *event )
{
    /* FIXME: with -desktop xxx we get this event _before_ desktop 
     * window structure is created. WIN_GetDesktop() check is a hack.
     */

    if ( !WIN_GetDesktop() || hwnd == GetDesktopWindow())
    {
        desktopX = event->x;
	desktopY = event->y;
    }
    else
    {
        WND *wndPtr;
	WINDOWPOS16 *winpos;
	RECT16 newWindowRect, newClientRect;
        HRGN hrgnOldPos, hrgnNewPos;

	if (!(wndPtr = WIN_FindWndPtr( hwnd )) ||
	    !(wndPtr->flags & WIN_MANAGED) )
	    return;
	
        if (!(winpos = SEGPTR_NEW(WINDOWPOS16))) return;


	/* Fill WINDOWPOS struct */
	winpos->flags = SWP_NOACTIVATE | SWP_NOZORDER;
	winpos->hwnd = hwnd;
	winpos->x = event->x;
	winpos->y = event->y;
	winpos->cx = event->width;
	winpos->cy = event->height;

	/* Check for unchanged attributes */
	if(winpos->x == wndPtr->rectWindow.left &&
	   winpos->y == wndPtr->rectWindow.top)
	    winpos->flags |= SWP_NOMOVE;
	if(winpos->cx == wndPtr->rectWindow.right - wndPtr->rectWindow.left &&
	   winpos->cy == wndPtr->rectWindow.bottom - wndPtr->rectWindow.top)
	    winpos->flags |= SWP_NOSIZE;

	/* Send WM_WINDOWPOSCHANGING */
	SendMessage16(hwnd, WM_WINDOWPOSCHANGING, 0, (LPARAM)SEGPTR_GET(winpos));

	/* Calculate new position and size */
	newWindowRect.left = event->x;
	newWindowRect.right = event->x + event->width;
	newWindowRect.top = event->y;
	newWindowRect.bottom = event->y + event->height;

	WINPOS_SendNCCalcSize( winpos->hwnd, TRUE, &newWindowRect,
			       &wndPtr->rectWindow, &wndPtr->rectClient,
			       SEGPTR_GET(winpos), &newClientRect );

        hrgnOldPos = CreateRectRgnIndirect16( &wndPtr->rectWindow );
        hrgnNewPos = CreateRectRgnIndirect16( &newWindowRect );
        CombineRgn( hrgnOldPos, hrgnOldPos, hrgnNewPos, RGN_DIFF );
 
	/* Set new size and position */
	wndPtr->rectWindow = newWindowRect;
	wndPtr->rectClient = newClientRect;
	SendMessage16( hwnd, WM_WINDOWPOSCHANGED, 0, (LPARAM)SEGPTR_GET(winpos));
        SEGPTR_FREE(winpos);

        /* full window drag leaves unrepainted garbage without this */
        PAINT_RedrawWindow( 0, NULL, hrgnOldPos, RDW_INVALIDATE |
                            RDW_ALLCHILDREN | RDW_ERASE | RDW_ERASENOW,
                            RDW_C_USEHRGN );
        DeleteObject(hrgnOldPos);
        DeleteObject(hrgnNewPos);
    }
}


/***********************************************************************
 *           EVENT_SelectionRequest
 */
static void EVENT_SelectionRequest( WND *pWnd, XSelectionRequestEvent *event )
{
    XSelectionEvent result;
    Atom 	    rprop = None;
    Window 	    request = event->requestor;

    if(event->target == XA_STRING)
    {
	HANDLE hText;
	LPSTR text;

        rprop = event->property;

	if(rprop == None) rprop = event->target;

        if(event->selection!=XA_PRIMARY) rprop = None;
        else if(!CLIPBOARD_IsPresent(CF_TEXT)) rprop = None;
	else{
            /* Don't worry if we can't open */
	    BOOL couldOpen=OpenClipboard( pWnd->hwndSelf );
	    hText=GetClipboardData(CF_TEXT);
	    text=GlobalLock16(hText);
	    XChangeProperty(display,request,rprop,XA_STRING,
		8,PropModeReplace,text,strlen(text));
	    GlobalUnlock16(hText);
	    /* close only if we opened before */
	    if(couldOpen)CloseClipboard();
	}
    }

    if(rprop==None) 
       dprintf_event(stddeb,"Request for %s ignored\n", XGetAtomName(display,event->target));

    result.type=SelectionNotify;
    result.display=display;
    result.requestor=request;
    result.selection=event->selection;
    result.property=rprop;
    result.target=event->target;
    result.time=event->time;
    XSendEvent(display,event->requestor,False,NoEventMask,(XEvent*)&result);
}


/***********************************************************************
 *           EVENT_SelectionNotify
 */
static void EVENT_SelectionNotify( XSelectionEvent *event )
{
    if (event->selection != XA_PRIMARY) return;
    if (event->target != XA_STRING) CLIPBOARD_ReadSelection( 0, None );
    CLIPBOARD_ReadSelection( event->requestor, event->property );
}


/***********************************************************************
 *           EVENT_SelectionClear
 */
static void EVENT_SelectionClear( WND *pWnd, XSelectionClearEvent *event )
{
    if (event->selection != XA_PRIMARY) return;
    CLIPBOARD_ReleaseSelection( pWnd->hwndSelf ); 
}


/**********************************************************************
 *           EVENT_ClientMessage
 */
static void EVENT_ClientMessage( WND *pWnd, XClientMessageEvent *event )
{
    static Atom wmProtocols = None;
    static Atom wmDeleteWindow = None;

    if (wmProtocols == None)
        wmProtocols = XInternAtom( display, "WM_PROTOCOLS", True );
    if (wmDeleteWindow == None)
        wmDeleteWindow = XInternAtom( display, "WM_DELETE_WINDOW", True );

    if ((event->format != 32) || (event->message_type != wmProtocols) ||
        (((Atom) event->data.l[0]) != wmDeleteWindow))
    {
	dprintf_event( stddeb, "unrecognized ClientMessage\n" );
	return;
    }
    SendMessage16( pWnd->hwndSelf, WM_SYSCOMMAND, SC_CLOSE, 0 );
}

/**********************************************************************
 *           EVENT_EnterNotify
 *
 * Install colormap when Wine window is focused in
 * self-managed mode with private colormap
 */
/*
  void EVENT_EnterNotify( WND *pWnd, XCrossingEvent *event )
  {
   if( !Options.managed && rootWindow == DefaultRootWindow(display) &&
     (COLOR_GetSystemPaletteFlags() & COLOR_PRIVATE) && GetFocus32() )
      XInstallColormap( display, COLOR_GetColormap() );
  }
 */ 

/**********************************************************************
 *		EVENT_MapNotify
 */
void EVENT_MapNotify( HWND hWnd, XMapEvent *event )
{
    HWND32 hwndFocus = GetFocus32();

    if (hwndFocus && IsChild( hWnd, hwndFocus ))
      FOCUS_SetXFocus( hwndFocus );

    return;
}

/**********************************************************************
 *		SetCapture 	(USER.18)
 */
HWND SetCapture( HWND hwnd )
{
    Window win;
    HWND old_capture_wnd = captureWnd;

    if (!hwnd)
    {
        ReleaseCapture();
        return old_capture_wnd;
    }
    if (!(win = WIN_GetXWindow( hwnd ))) return 0;
    if (XGrabPointer(display, win, False, 
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime ) == GrabSuccess)
    {
	dprintf_win(stddeb, "SetCapture: %04x\n", hwnd);
	captureWnd   = hwnd;
	return old_capture_wnd;
    }
    else return 0;
}


/**********************************************************************
 *		ReleaseCapture	(USER.19)
 */
void ReleaseCapture()
{
    if (captureWnd == 0) return;
    XUngrabPointer( display, CurrentTime );
    captureWnd = 0;
    dprintf_win(stddeb, "ReleaseCapture\n");
}

/**********************************************************************
 *		GetCapture 	(USER.236)
 */
HWND GetCapture()
{
    return captureWnd;
}


/***********************************************************************
 *           GetMouseEventProc   (USER.337)
 */
FARPROC16 GetMouseEventProc(void)
{
    HMODULE16 hmodule = GetModuleHandle("USER");
    return MODULE_GetEntryPoint( hmodule,
                                 MODULE_GetOrdinal( hmodule, "Mouse_Event" ) );
}


/***********************************************************************
 *           Mouse_Event   (USER.299)
 */
#ifndef WINELIB
void Mouse_Event( SIGCONTEXT *context )
{
    /* Register values:
     * AX = mouse event
     * BX = horizontal displacement if AX & ME_MOVE
     * CX = vertical displacement if AX & ME_MOVE
     * DX = button state (?)
     * SI = mouse event flags (?)
     */
    Window root, child;
    int rootX, rootY, childX, childY;
    unsigned int state;

    if (AX_reg(context) & ME_MOVE)
    {
        /* We have to actually move the cursor */
        XWarpPointer( display, rootWindow, None, 0, 0, 0, 0,
                      (short)BX_reg(context), (short)CX_reg(context) );
        return;
    }
    if (!XQueryPointer( display, rootWindow, &root, &child,
                        &rootX, &rootY, &childX, &childY, &state )) return;
    if (AX_reg(context) & ME_LDOWN)
        hardware_event( WM_LBUTTONDOWN, EVENT_XStateToKeyState( state ), 0L,
                        rootX - desktopX, rootY - desktopY, GetTickCount(), 0);
    if (AX_reg(context) & ME_LUP)
        hardware_event( WM_LBUTTONUP, EVENT_XStateToKeyState( state ), 0L,
                        rootX - desktopX, rootY - desktopY, GetTickCount(), 0);
    if (AX_reg(context) & ME_RDOWN)
        hardware_event( WM_RBUTTONDOWN, EVENT_XStateToKeyState( state ), 0L,
                        rootX - desktopX, rootY - desktopY, GetTickCount(), 0);
    if (AX_reg(context) & ME_RUP)
        hardware_event( WM_RBUTTONUP, EVENT_XStateToKeyState( state ), 0L,
                        rootX - desktopX, rootY - desktopY, GetTickCount(), 0);
}
#endif


/**********************************************************************
 *			EnableHardwareInput 		[USER.331]
 */
BOOL EnableHardwareInput(BOOL bEnable)
{
  BOOL bOldState = InputEnabled;
  dprintf_event(stdnimp,"EMPTY STUB !!! EnableHardwareInput(%d);\n", bEnable);
  InputEnabled = bEnable;
  return (bOldState && !bEnable);
}

