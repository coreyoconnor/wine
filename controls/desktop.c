/*
 * Desktop window class.
 *
 * Copyright 1994 Alexandre Julliard
 */

#include "x11drv.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "desktop.h"
#include "heap.h"
#include "monitor.h"
#include "win.h"
#include "wine/winuser16.h"

/***********************************************************************
 *              DESKTOP_GetScreenWidth
 *
 * Return the width of the screen associated to the current desktop.
 */
int DESKTOP_GetScreenWidth()
{
  DESKTOP *pDesktop = (DESKTOP *) WIN_GetDesktop()->wExtra;
  return MONITOR_GetWidth(pDesktop->pPrimaryMonitor);
}

/***********************************************************************
 *              DESKTOP_GetScreenHeight
 *
 * Return the height of the screen associated to the current desktop.
 */
int DESKTOP_GetScreenHeight()
{
  DESKTOP *pDesktop = (DESKTOP *) WIN_GetDesktop()->wExtra;
  return MONITOR_GetHeight(pDesktop->pPrimaryMonitor);
}

/***********************************************************************
 *              DESKTOP_GetScreenDepth
 *
 * Return the depth of the screen associated to the current desktop.
 */
int DESKTOP_GetScreenDepth()
{
  DESKTOP *pDesktop = (DESKTOP *) WIN_GetDesktop()->wExtra;
  return MONITOR_GetDepth(pDesktop->pPrimaryMonitor);
}

/***********************************************************************
 *           DESKTOP_LoadBitmap
 *
 * Load a bitmap from a file. Used by SetDeskWallPaper().
 */
static HBITMAP32 DESKTOP_LoadBitmap( HDC32 hdc, const char *filename )
{
    BITMAPFILEHEADER *fileHeader;
    BITMAPINFO *bitmapInfo;
    HBITMAP32 hbitmap;
    HFILE32 file;
    LPSTR buffer;
    LONG size;

    /* Read all the file into memory */

    if ((file = _lopen32( filename, OF_READ )) == HFILE_ERROR32)
    {
        UINT32 len = GetWindowsDirectory32A( NULL, 0 );
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                                  len + strlen(filename) + 2 )))
            return 0;
        GetWindowsDirectory32A( buffer, len + 1 );
        strcat( buffer, "\\" );
        strcat( buffer, filename );
        file = _lopen32( buffer, OF_READ );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
    if (file == HFILE_ERROR32) return 0;
    size = _llseek32( file, 0, 2 );
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0, size )))
    {
	_lclose32( file );
	return 0;
    }
    _llseek32( file, 0, 0 );
    size = _lread32( file, buffer, size );
    _lclose32( file );
    fileHeader = (BITMAPFILEHEADER *)buffer;
    bitmapInfo = (BITMAPINFO *)(buffer + sizeof(BITMAPFILEHEADER));
    
      /* Check header content */
    if ((fileHeader->bfType != 0x4d42) || (size < fileHeader->bfSize))
    {
	HeapFree( GetProcessHeap(), 0, buffer );
	return 0;
    }
    hbitmap = CreateDIBitmap32( hdc, &bitmapInfo->bmiHeader, CBM_INIT,
                                buffer + fileHeader->bfOffBits,
                                bitmapInfo, DIB_RGB_COLORS );
    HeapFree( GetProcessHeap(), 0, buffer );
    return hbitmap;
}


/***********************************************************************
 *           DESKTOP_DoEraseBkgnd
 *
 * Handle the WM_ERASEBKGND message.
 */
static LRESULT DESKTOP_DoEraseBkgnd( HWND32 hwnd, HDC32 hdc,
                                     DESKTOP *desktopPtr )
{
    RECT32 rect;
    WND*   Wnd = WIN_FindWndPtr( hwnd );

    if (Wnd->hrgnUpdate > 1) DeleteObject32( Wnd->hrgnUpdate );
    Wnd->hrgnUpdate = 0;

    GetClientRect32( hwnd, &rect );    

    /* Paint desktop pattern (only if wall paper does not cover everything) */

    if (!desktopPtr->hbitmapWallPaper || 
	(!desktopPtr->fTileWallPaper && ((desktopPtr->bitmapSize.cx < rect.right) ||
	 (desktopPtr->bitmapSize.cy < rect.bottom))))
    {
	  /* Set colors in case pattern is a monochrome bitmap */
	SetBkColor32( hdc, RGB(0,0,0) );
	SetTextColor32( hdc, GetSysColor32(COLOR_BACKGROUND) );
	FillRect32( hdc, &rect, desktopPtr->hbrushPattern );
    }

      /* Paint wall paper */

    if (desktopPtr->hbitmapWallPaper)
    {
	INT32 x, y;
	HDC32 hMemDC = CreateCompatibleDC32( hdc );
	
	SelectObject32( hMemDC, desktopPtr->hbitmapWallPaper );

	if (desktopPtr->fTileWallPaper)
	{
	    for (y = 0; y < rect.bottom; y += desktopPtr->bitmapSize.cy)
		for (x = 0; x < rect.right; x += desktopPtr->bitmapSize.cx)
		    BitBlt32( hdc, x, y, desktopPtr->bitmapSize.cx,
			      desktopPtr->bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
	}
	else
	{
	    x = (rect.left + rect.right - desktopPtr->bitmapSize.cx) / 2;
	    y = (rect.top + rect.bottom - desktopPtr->bitmapSize.cy) / 2;
	    if (x < 0) x = 0;
	    if (y < 0) y = 0;
	    BitBlt32( hdc, x, y, desktopPtr->bitmapSize.cx,
		      desktopPtr->bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
	}
	DeleteDC32( hMemDC );
    }

    return 1;
}


/***********************************************************************
 *           DesktopWndProc
 *
 * Window procedure for the desktop window.
 */
LRESULT WINAPI DesktopWndProc( HWND32 hwnd, UINT32 message,
                               WPARAM32 wParam, LPARAM lParam )
{
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    DESKTOP *desktopPtr = (DESKTOP *)wndPtr->wExtra;

      /* Most messages are ignored (we DON'T call DefWindowProc) */

    switch(message)
    {
	/* Warning: this message is sent directly by                     */
	/* WIN_CreateDesktopWindow() and does not contain a valid lParam */
    case WM_NCCREATE:
	desktopPtr->hbrushPattern = 0;
	desktopPtr->hbitmapWallPaper = 0;
	SetDeskPattern();
	SetDeskWallPaper32( (LPSTR)-1 );
	return 1;
	
    case WM_ERASEBKGND:
	if (X11DRV_WND_GetXRootWindow(wndPtr) == 
	    DefaultRootWindow(display))
	  return 1;
	return DESKTOP_DoEraseBkgnd( hwnd, (HDC32)wParam, desktopPtr );

    case WM_SYSCOMMAND:
	if ((wParam & 0xfff0) != SC_CLOSE) return 0;
	ExitWindows16( 0, 0 ); 

    case WM_SETCURSOR:
        return (LRESULT)SetCursor16( LoadCursor16( 0, IDC_ARROW16 ) );
    }
    
    return 0;
}

/***********************************************************************
 *           PaintDesktop   (USER32.415)
 *
 */
BOOL32 WINAPI PaintDesktop(HDC32 hdc)
{
    HWND32 hwnd = GetDesktopWindow32();
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    DESKTOP *desktopPtr = (DESKTOP *)wndPtr->wExtra;

    return DESKTOP_DoEraseBkgnd( hwnd, hdc, desktopPtr );
}

/***********************************************************************
 *           SetDeskPattern   (USER.279)
 */
BOOL16 WINAPI SetDeskPattern(void)
{
    char buffer[100];
    GetProfileString32A( "desktop", "Pattern", "(None)", buffer, 100 );
    return DESKTOP_SetPattern( buffer );
}


/***********************************************************************
 *           SetDeskWallPaper16   (USER.285)
 */
BOOL16 WINAPI SetDeskWallPaper16( LPCSTR filename )
{
    return SetDeskWallPaper32( filename );
}


/***********************************************************************
 *           SetDeskWallPaper32   (USER32.475)
 *
 * FIXME: is there a unicode version?
 */
BOOL32 WINAPI SetDeskWallPaper32( LPCSTR filename )
{
    HBITMAP32 hbitmap;
    HDC32 hdc;
    char buffer[256];
    WND *wndPtr = WIN_GetDesktop();
    DESKTOP *desktopPtr = (DESKTOP *)wndPtr->wExtra;

    if (filename == (LPSTR)-1)
    {
	GetProfileString32A( "desktop", "WallPaper", "(None)", buffer, 256 );
	filename = buffer;
    }
    hdc = GetDC32( 0 );
    hbitmap = DESKTOP_LoadBitmap( hdc, filename );
    ReleaseDC32( 0, hdc );
    if (desktopPtr->hbitmapWallPaper) DeleteObject32( desktopPtr->hbitmapWallPaper );
    desktopPtr->hbitmapWallPaper = hbitmap;
    desktopPtr->fTileWallPaper = GetProfileInt32A( "desktop", "TileWallPaper", 0 );
    if (hbitmap)
    {
	BITMAP32 bmp;
	GetObject32A( hbitmap, sizeof(bmp), &bmp );
	desktopPtr->bitmapSize.cx = (bmp.bmWidth != 0) ? bmp.bmWidth : 1;
	desktopPtr->bitmapSize.cy = (bmp.bmHeight != 0) ? bmp.bmHeight : 1;
    }
    return TRUE;
}


/***********************************************************************
 *           DESKTOP_SetPattern
 *
 * Set the desktop pattern.
 */
BOOL32 DESKTOP_SetPattern( LPCSTR pattern )
{
    WND *wndPtr = WIN_GetDesktop();
    DESKTOP *desktopPtr = (DESKTOP *)wndPtr->wExtra;
    int pat[8];

    if (desktopPtr->hbrushPattern) DeleteObject32( desktopPtr->hbrushPattern );
    memset( pat, 0, sizeof(pat) );
    if (pattern && sscanf( pattern, " %d %d %d %d %d %d %d %d",
			   &pat[0], &pat[1], &pat[2], &pat[3],
			   &pat[4], &pat[5], &pat[6], &pat[7] ))
    {
	WORD pattern[8];
	HBITMAP32 hbitmap;
	int i;

	for (i = 0; i < 8; i++) pattern[i] = pat[i] & 0xffff;
	hbitmap = CreateBitmap32( 8, 8, 1, 1, (LPSTR)pattern );
	desktopPtr->hbrushPattern = CreatePatternBrush32( hbitmap );
	DeleteObject32( hbitmap );
    }
    else desktopPtr->hbrushPattern = CreateSolidBrush32( GetSysColor32(COLOR_BACKGROUND) );
    return TRUE;
}

