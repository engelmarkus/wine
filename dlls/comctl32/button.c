/*
 * Copyright (C) 1993 Johannes Ruscheinski
 * Copyright (C) 1993 David Metcalfe
 * Copyright (C) 1994 Alexandre Julliard
 * Copyright (C) 2008 by Reece H. Dunn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * TODO
 *  Styles
 *  - BS_NOTIFY: is it complete?
 *  - BS_RIGHTBUTTON: same as BS_LEFTTEXT
 *
 *  Messages
 *  - WM_CHAR: Checks a (manual or automatic) check box on '+' or '=', clears it on '-' key.
 *  - WM_SETFOCUS: For (manual or automatic) radio buttons, send the parent window BN_CLICKED
 *  - WM_NCCREATE: Turns any BS_OWNERDRAW button into a BS_PUSHBUTTON button.
 *  - WM_SYSKEYUP
 *  - BCM_GETIDEALSIZE
 *
 *  Notifications
 *  - BCN_HOTITEMCHANGE
 *  - BN_DISABLE
 *  - BN_PUSHED/BN_HILITE
 *  + BN_KILLFOCUS: is it OK?
 *  - BN_PAINT
 *  + BN_SETFOCUS: is it OK?
 *  - BN_UNPUSHED/BN_UNHILITE
 *  - NM_CUSTOMDRAW
 *
 *  Structures/Macros/Definitions
 *  - NMBCHOTITEM
 *  - Button_GetIdealSize
 */

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define OEMRESOURCE

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "uxtheme.h"
#include "vssym32.h"
#include "wine/debug.h"
#include "wine/heap.h"

#include "comctl32.h"

WINE_DEFAULT_DEBUG_CHANNEL(button);

/* undocumented flags */
#define BUTTON_NSTATES         0x0F
#define BUTTON_BTNPRESSED      0x40
#define BUTTON_UNKNOWN2        0x20
#define BUTTON_UNKNOWN3        0x10

#define BUTTON_NOTIFY_PARENT(hWnd, code) \
    do { /* Notify parent which has created this button control */ \
        TRACE("notification " #code " sent to hwnd=%p\n", GetParent(hWnd)); \
        SendMessageW(GetParent(hWnd), WM_COMMAND, \
                     MAKEWPARAM(GetWindowLongPtrW((hWnd),GWLP_ID), (code)), \
                     (LPARAM)(hWnd)); \
    } while(0)

typedef struct _BUTTON_INFO
{
    HWND             hwnd;
    HWND             parent;
    LONG             style;
    LONG             state;
    HFONT            font;
    WCHAR           *note;
    INT              note_length;
    DWORD            image_type; /* IMAGE_BITMAP or IMAGE_ICON */
    BUTTON_IMAGELIST imagelist;
    RECT             text_margin;
    union
    {
        HICON   icon;
        HBITMAP bitmap;
        HANDLE  image;
    } u;
} BUTTON_INFO;

static UINT BUTTON_CalcLayoutRects( const BUTTON_INFO *infoPtr, HDC hdc, RECT *labelRc, RECT *imageRc, RECT *textRc );
static void PB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action );
static void CB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action );
static void GB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action );
static void UB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action );
static void OB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action );
static void BUTTON_CheckAutoRadioButton( HWND hwnd );

#define MAX_BTN_TYPE  16

static const WORD maxCheckState[MAX_BTN_TYPE] =
{
    BST_UNCHECKED,      /* BS_PUSHBUTTON */
    BST_UNCHECKED,      /* BS_DEFPUSHBUTTON */
    BST_CHECKED,        /* BS_CHECKBOX */
    BST_CHECKED,        /* BS_AUTOCHECKBOX */
    BST_CHECKED,        /* BS_RADIOBUTTON */
    BST_INDETERMINATE,  /* BS_3STATE */
    BST_INDETERMINATE,  /* BS_AUTO3STATE */
    BST_UNCHECKED,      /* BS_GROUPBOX */
    BST_UNCHECKED,      /* BS_USERBUTTON */
    BST_CHECKED,        /* BS_AUTORADIOBUTTON */
    BST_UNCHECKED,      /* BS_PUSHBOX */
    BST_UNCHECKED,      /* BS_OWNERDRAW */
    BST_UNCHECKED,      /* BS_SPLITBUTTON */
    BST_UNCHECKED,      /* BS_DEFSPLITBUTTON */
    BST_UNCHECKED,      /* BS_COMMANDLINK */
    BST_UNCHECKED       /* BS_DEFCOMMANDLINK */
};

/* Generic draw states, use get_draw_state() to get specific state for button type */
enum draw_state
{
    STATE_NORMAL,
    STATE_DISABLED,
    STATE_HOT,
    STATE_PRESSED,
    STATE_DEFAULTED,
    DRAW_STATE_COUNT
};

typedef void (*pfPaint)( const BUTTON_INFO *infoPtr, HDC hdc, UINT action );

static const pfPaint btnPaintFunc[MAX_BTN_TYPE] =
{
    PB_Paint,    /* BS_PUSHBUTTON */
    PB_Paint,    /* BS_DEFPUSHBUTTON */
    CB_Paint,    /* BS_CHECKBOX */
    CB_Paint,    /* BS_AUTOCHECKBOX */
    CB_Paint,    /* BS_RADIOBUTTON */
    CB_Paint,    /* BS_3STATE */
    CB_Paint,    /* BS_AUTO3STATE */
    GB_Paint,    /* BS_GROUPBOX */
    UB_Paint,    /* BS_USERBUTTON */
    CB_Paint,    /* BS_AUTORADIOBUTTON */
    NULL,        /* BS_PUSHBOX */
    OB_Paint,    /* BS_OWNERDRAW */
    PB_Paint,    /* BS_SPLITBUTTON */
    PB_Paint,    /* BS_DEFSPLITBUTTON */
    PB_Paint,    /* BS_COMMANDLINK */
    PB_Paint     /* BS_DEFCOMMANDLINK */
};

typedef void (*pfThemedPaint)( HTHEME theme, const BUTTON_INFO *infoPtr, HDC hdc, int drawState, UINT dtflags, BOOL focused);

static void PB_ThemedPaint( HTHEME theme, const BUTTON_INFO *infoPtr, HDC hdc, int drawState, UINT dtflags, BOOL focused);
static void CB_ThemedPaint( HTHEME theme, const BUTTON_INFO *infoPtr, HDC hdc, int drawState, UINT dtflags, BOOL focused);
static void GB_ThemedPaint( HTHEME theme, const BUTTON_INFO *infoPtr, HDC hdc, int drawState, UINT dtflags, BOOL focused);

static const pfThemedPaint btnThemedPaintFunc[MAX_BTN_TYPE] =
{
    PB_ThemedPaint, /* BS_PUSHBUTTON */
    PB_ThemedPaint, /* BS_DEFPUSHBUTTON */
    CB_ThemedPaint, /* BS_CHECKBOX */
    CB_ThemedPaint, /* BS_AUTOCHECKBOX */
    CB_ThemedPaint, /* BS_RADIOBUTTON */
    CB_ThemedPaint, /* BS_3STATE */
    CB_ThemedPaint, /* BS_AUTO3STATE */
    GB_ThemedPaint, /* BS_GROUPBOX */
    NULL,           /* BS_USERBUTTON */
    CB_ThemedPaint, /* BS_AUTORADIOBUTTON */
    NULL,           /* BS_PUSHBOX */
    NULL,           /* BS_OWNERDRAW */
    NULL,           /* BS_SPLITBUTTON */
    NULL,           /* BS_DEFSPLITBUTTON */
    NULL,           /* BS_COMMANDLINK */
    NULL,           /* BS_DEFCOMMANDLINK */
};

static inline UINT get_button_type( LONG window_style )
{
    return (window_style & BS_TYPEMASK);
}

/* paint a button of any type */
static inline void paint_button( BUTTON_INFO *infoPtr, LONG style, UINT action )
{
    if (btnPaintFunc[style] && IsWindowVisible(infoPtr->hwnd))
    {
        HDC hdc = GetDC( infoPtr->hwnd );
        btnPaintFunc[style]( infoPtr, hdc, action );
        ReleaseDC( infoPtr->hwnd, hdc );
    }
}

/* retrieve the button text; returned buffer must be freed by caller */
static inline WCHAR *get_button_text( const BUTTON_INFO *infoPtr )
{
    INT len = GetWindowTextLengthW( infoPtr->hwnd );
    WCHAR *buffer = heap_alloc( (len + 1) * sizeof(WCHAR) );
    if (buffer)
        GetWindowTextW( infoPtr->hwnd, buffer, len + 1 );
    return buffer;
}

HRGN set_control_clipping( HDC hdc, const RECT *rect )
{
    RECT rc = *rect;
    HRGN hrgn = CreateRectRgn( 0, 0, 0, 0 );

    if (GetClipRgn( hdc, hrgn ) != 1)
    {
        DeleteObject( hrgn );
        hrgn = 0;
    }
    DPtoLP( hdc, (POINT *)&rc, 2 );
    if (GetLayout( hdc ) & LAYOUT_RTL)  /* compensate for the shifting done by IntersectClipRect */
    {
        rc.left++;
        rc.right++;
    }
    IntersectClipRect( hdc, rc.left, rc.top, rc.right, rc.bottom );
    return hrgn;
}

static WCHAR *heap_strndupW(const WCHAR *src, size_t length)
{
    size_t size = (length + 1) * sizeof(WCHAR);
    WCHAR *dst = heap_alloc(size);
    if (dst) memcpy(dst, src, size);
    return dst;
}

/**********************************************************************
 * Convert button styles to flags used by DrawText.
 */
static UINT BUTTON_BStoDT( DWORD style, DWORD ex_style )
{
    UINT dtStyle = DT_NOCLIP;  /* We use SelectClipRgn to limit output */

    /* "Convert" pushlike buttons to pushbuttons */
    if (style & BS_PUSHLIKE)
        style &= ~BS_TYPEMASK;

    if (!(style & BS_MULTILINE))
        dtStyle |= DT_SINGLELINE;
    else
        dtStyle |= DT_WORDBREAK;

    switch (style & BS_CENTER)
    {
        case BS_LEFT:   /* DT_LEFT is 0 */    break;
        case BS_RIGHT:  dtStyle |= DT_RIGHT;  break;
        case BS_CENTER: dtStyle |= DT_CENTER; break;
        default:
            /* Pushbutton's text is centered by default */
            if (get_button_type(style) <= BS_DEFPUSHBUTTON) dtStyle |= DT_CENTER;
            /* all other flavours have left aligned text */
    }

    if (ex_style & WS_EX_RIGHT) dtStyle = DT_RIGHT | (dtStyle & ~(DT_LEFT | DT_CENTER));

    /* DrawText ignores vertical alignment for multiline text,
     * but we use these flags to align label manually.
     */
    if (get_button_type(style) != BS_GROUPBOX)
    {
        switch (style & BS_VCENTER)
        {
            case BS_TOP:     /* DT_TOP is 0 */      break;
            case BS_BOTTOM:  dtStyle |= DT_BOTTOM;  break;
            case BS_VCENTER: /* fall through */
            default:         dtStyle |= DT_VCENTER; break;
        }
    }

    return dtStyle;
}

static int get_draw_state(const BUTTON_INFO *infoPtr)
{
    static const int pb_states[DRAW_STATE_COUNT] = { PBS_NORMAL, PBS_DISABLED, PBS_HOT, PBS_PRESSED, PBS_DEFAULTED };
    static const int cb_states[3][DRAW_STATE_COUNT] =
    {
        { CBS_UNCHECKEDNORMAL, CBS_UNCHECKEDDISABLED, CBS_UNCHECKEDHOT, CBS_UNCHECKEDPRESSED, CBS_UNCHECKEDNORMAL },
        { CBS_CHECKEDNORMAL, CBS_CHECKEDDISABLED, CBS_CHECKEDHOT, CBS_CHECKEDPRESSED, CBS_CHECKEDNORMAL },
        { CBS_MIXEDNORMAL, CBS_MIXEDDISABLED, CBS_MIXEDHOT, CBS_MIXEDPRESSED, CBS_MIXEDNORMAL }
    };
    static const int rb_states[2][DRAW_STATE_COUNT] =
    {
        { RBS_UNCHECKEDNORMAL, RBS_UNCHECKEDDISABLED, RBS_UNCHECKEDHOT, RBS_UNCHECKEDPRESSED, RBS_UNCHECKEDNORMAL },
        { RBS_CHECKEDNORMAL, RBS_CHECKEDDISABLED, RBS_CHECKEDHOT, RBS_CHECKEDPRESSED, RBS_CHECKEDNORMAL }
    };
    static const int gb_states[DRAW_STATE_COUNT] = { GBS_NORMAL, GBS_DISABLED, GBS_NORMAL, GBS_NORMAL, GBS_NORMAL };
    LONG style = GetWindowLongW(infoPtr->hwnd, GWL_STYLE);
    UINT type = get_button_type(style);
    int check_state = infoPtr->state & 3;
    enum draw_state state;

    if (!IsWindowEnabled(infoPtr->hwnd))
        state = STATE_DISABLED;
    else if (infoPtr->state & BST_PUSHED)
        state = STATE_PRESSED;
    else if (infoPtr->state & BST_HOT)
        state = STATE_HOT;
    else if (infoPtr->state & BST_FOCUS)
        state = STATE_DEFAULTED;
    else
        state = STATE_NORMAL;

    switch (type)
    {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
    case BS_USERBUTTON:
    case BS_SPLITBUTTON:
    case BS_DEFSPLITBUTTON:
    case BS_COMMANDLINK:
    case BS_DEFCOMMANDLINK:
        return pb_states[state];
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
        return cb_states[check_state][state];
    case BS_RADIOBUTTON:
    case BS_3STATE:
    case BS_AUTO3STATE:
    case BS_AUTORADIOBUTTON:
        return rb_states[check_state][state];
    case BS_GROUPBOX:
        return gb_states[state];
    default:
        WARN("Unsupported button type 0x%08x\n", type);
        return PBS_NORMAL;
    }
}

static LRESULT CALLBACK BUTTON_WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    BUTTON_INFO *infoPtr = (BUTTON_INFO *)GetWindowLongPtrW(hWnd, 0);
    RECT rect;
    POINT pt;
    LONG style = GetWindowLongW( hWnd, GWL_STYLE );
    UINT btn_type = get_button_type( style );
    LONG state, new_state;
    HANDLE oldHbitmap;
    HTHEME theme;

    if (!IsWindow( hWnd )) return 0;

    if (!infoPtr && (uMsg != WM_NCCREATE))
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);

    pt.x = (short)LOWORD(lParam);
    pt.y = (short)HIWORD(lParam);

    switch (uMsg)
    {
    case WM_GETDLGCODE:
        switch(btn_type)
        {
        case BS_COMMANDLINK:
        case BS_USERBUTTON:
        case BS_PUSHBUTTON:      return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        case BS_DEFCOMMANDLINK:
        case BS_DEFPUSHBUTTON:   return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON: return DLGC_BUTTON | DLGC_RADIOBUTTON;
        case BS_GROUPBOX:        return DLGC_STATIC;
        case BS_SPLITBUTTON:     return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_WANTARROWS;
        case BS_DEFSPLITBUTTON:  return DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_WANTARROWS;
        default:                 return DLGC_BUTTON;
        }

    case WM_ENABLE:
        theme = GetWindowTheme( hWnd );
        if (theme)
            RedrawWindow( hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW );
        else
            paint_button( infoPtr, btn_type, ODA_DRAWENTIRE );
        break;

    case WM_NCCREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;

        infoPtr = heap_alloc_zero( sizeof(*infoPtr) );
        SetWindowLongPtrW( hWnd, 0, (LONG_PTR)infoPtr );
        infoPtr->hwnd = hWnd;
        infoPtr->parent = cs->hwndParent;
        infoPtr->style = cs->style;
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    case WM_NCDESTROY:
        SetWindowLongPtrW( hWnd, 0, 0 );
        heap_free(infoPtr->note);
        heap_free(infoPtr);
        break;

    case WM_CREATE:
        if (btn_type >= MAX_BTN_TYPE)
            return -1; /* abort */

        /* XP turns a BS_USERBUTTON into BS_PUSHBUTTON */
        if (btn_type == BS_USERBUTTON )
        {
            style = (style & ~BS_TYPEMASK) | BS_PUSHBUTTON;
            SetWindowLongW( hWnd, GWL_STYLE, style );
        }
        infoPtr->state = BST_UNCHECKED;
        OpenThemeData( hWnd, WC_BUTTONW );
        return 0;

    case WM_DESTROY:
        theme = GetWindowTheme( hWnd );
        CloseThemeData( theme );
        break;

    case WM_THEMECHANGED:
        theme = GetWindowTheme( hWnd );
        CloseThemeData( theme );
        OpenThemeData( hWnd, WC_BUTTONW );
        break;

    case WM_ERASEBKGND:
        if (btn_type == BS_OWNERDRAW)
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            HBRUSH hBrush;
            HWND parent = GetParent(hWnd);
            if (!parent) parent = hWnd;
            hBrush = (HBRUSH)SendMessageW(parent, WM_CTLCOLORBTN, (WPARAM)hdc, (LPARAM)hWnd);
            if (!hBrush) /* did the app forget to call defwindowproc ? */
                hBrush = (HBRUSH)DefWindowProcW(parent, WM_CTLCOLORBTN,
                                                (WPARAM)hdc, (LPARAM)hWnd);
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, hBrush);
        }
        return 1;

    case WM_PRINTCLIENT:
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;

        theme = GetWindowTheme( hWnd );
        hdc = wParam ? (HDC)wParam : BeginPaint( hWnd, &ps );

        if (theme && btnThemedPaintFunc[btn_type])
        {
            int drawState = get_draw_state(infoPtr);
            UINT dtflags = BUTTON_BStoDT(style, GetWindowLongW(hWnd, GWL_EXSTYLE));

            btnThemedPaintFunc[btn_type](theme, infoPtr, hdc, drawState, dtflags, infoPtr->state & BST_FOCUS);
        }
        else if (btnPaintFunc[btn_type])
        {
            int nOldMode = SetBkMode( hdc, OPAQUE );
            btnPaintFunc[btn_type]( infoPtr, hdc, ODA_DRAWENTIRE );
            SetBkMode(hdc, nOldMode); /*  reset painting mode */
        }

        if ( !wParam ) EndPaint( hWnd, &ps );
        break;
    }

    case WM_KEYDOWN:
	if (wParam == VK_SPACE)
	{
	    SendMessageW( hWnd, BM_SETSTATE, TRUE, 0 );
            infoPtr->state |= BUTTON_BTNPRESSED;
            SetCapture( hWnd );
	}
	break;

    case WM_LBUTTONDBLCLK:
        if(style & BS_NOTIFY ||
           btn_type == BS_RADIOBUTTON ||
           btn_type == BS_USERBUTTON ||
           btn_type == BS_OWNERDRAW)
        {
            BUTTON_NOTIFY_PARENT(hWnd, BN_DOUBLECLICKED);
            break;
        }
        /* fall through */
    case WM_LBUTTONDOWN:
        SetCapture( hWnd );
        SetFocus( hWnd );
        infoPtr->state |= BUTTON_BTNPRESSED;
        SendMessageW( hWnd, BM_SETSTATE, TRUE, 0 );
        break;

    case WM_KEYUP:
	if (wParam != VK_SPACE)
	    break;
	/* fall through */
    case WM_LBUTTONUP:
        state = infoPtr->state;
        if (!(state & BUTTON_BTNPRESSED)) break;
        infoPtr->state &= BUTTON_NSTATES;
        if (!(state & BST_PUSHED))
        {
            ReleaseCapture();
            break;
        }
        SendMessageW( hWnd, BM_SETSTATE, FALSE, 0 );
        GetClientRect( hWnd, &rect );
	if (uMsg == WM_KEYUP || PtInRect( &rect, pt ))
        {
            switch(btn_type)
            {
            case BS_AUTOCHECKBOX:
                SendMessageW( hWnd, BM_SETCHECK, !(infoPtr->state & BST_CHECKED), 0 );
                break;
            case BS_AUTORADIOBUTTON:
                SendMessageW( hWnd, BM_SETCHECK, TRUE, 0 );
                break;
            case BS_AUTO3STATE:
                SendMessageW( hWnd, BM_SETCHECK, (infoPtr->state & BST_INDETERMINATE) ? 0 :
                    ((infoPtr->state & 3) + 1), 0 );
                break;
            }
            ReleaseCapture();
            BUTTON_NOTIFY_PARENT(hWnd, BN_CLICKED);
        }
        else
        {
            ReleaseCapture();
        }
        break;

    case WM_CAPTURECHANGED:
        TRACE("WM_CAPTURECHANGED %p\n", hWnd);
        if (hWnd == (HWND)lParam) break;
        if (infoPtr->state & BUTTON_BTNPRESSED)
        {
            infoPtr->state &= BUTTON_NSTATES;
            if (infoPtr->state & BST_PUSHED)
                SendMessageW( hWnd, BM_SETSTATE, FALSE, 0 );
        }
        break;

    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT mouse_event;

        mouse_event.cbSize = sizeof(TRACKMOUSEEVENT);
        mouse_event.dwFlags = TME_QUERY;
        if (!TrackMouseEvent(&mouse_event) || !(mouse_event.dwFlags & (TME_HOVER | TME_LEAVE)))
        {
            mouse_event.dwFlags = TME_HOVER | TME_LEAVE;
            mouse_event.hwndTrack = hWnd;
            mouse_event.dwHoverTime = 1;
            TrackMouseEvent(&mouse_event);
        }

        if ((wParam & MK_LBUTTON) && GetCapture() == hWnd)
        {
            GetClientRect( hWnd, &rect );
            SendMessageW( hWnd, BM_SETSTATE, PtInRect(&rect, pt), 0 );
        }
        break;
    }

    case WM_MOUSEHOVER:
    {
        infoPtr->state |= BST_HOT;
        InvalidateRect( hWnd, NULL, FALSE );
        break;
    }

    case WM_MOUSELEAVE:
    {
        infoPtr->state &= ~BST_HOT;
        InvalidateRect( hWnd, NULL, FALSE );
        break;
    }

    case WM_SETTEXT:
    {
        /* Clear an old text here as Windows does */
        if (IsWindowVisible(hWnd))
        {
            HDC hdc = GetDC(hWnd);
            HBRUSH hbrush;
            RECT client, rc;
            HWND parent = GetParent(hWnd);
            UINT message = (btn_type == BS_PUSHBUTTON ||
                            btn_type == BS_DEFPUSHBUTTON ||
                            btn_type == BS_USERBUTTON ||
                            btn_type == BS_OWNERDRAW) ?
                            WM_CTLCOLORBTN : WM_CTLCOLORSTATIC;

            if (!parent) parent = hWnd;
            hbrush = (HBRUSH)SendMessageW(parent, message,
                                          (WPARAM)hdc, (LPARAM)hWnd);
            if (!hbrush) /* did the app forget to call DefWindowProc ? */
                hbrush = (HBRUSH)DefWindowProcW(parent, message,
                                                (WPARAM)hdc, (LPARAM)hWnd);

            GetClientRect(hWnd, &client);
            rc = client;
            /* FIXME: check other BS_* handlers */
            if (btn_type == BS_GROUPBOX)
                InflateRect(&rc, -7, 1); /* GB_Paint does this */
            BUTTON_CalcLayoutRects(infoPtr, hdc, &rc, NULL, NULL);
            /* Clip by client rect bounds */
            if (rc.right > client.right) rc.right = client.right;
            if (rc.bottom > client.bottom) rc.bottom = client.bottom;
            FillRect(hdc, &rc, hbrush);
            ReleaseDC(hWnd, hdc);
        }

        DefWindowProcW( hWnd, WM_SETTEXT, wParam, lParam );
        if (btn_type == BS_GROUPBOX) /* Yes, only for BS_GROUPBOX */
            InvalidateRect( hWnd, NULL, TRUE );
        else
            paint_button( infoPtr, btn_type, ODA_DRAWENTIRE );
        return 1; /* success. FIXME: check text length */
    }

    case BCM_SETNOTE:
    {
        WCHAR *note = (WCHAR *)lParam;
        if (btn_type != BS_COMMANDLINK && btn_type != BS_DEFCOMMANDLINK)
        {
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }

        heap_free(infoPtr->note);
        if (note)
        {
            infoPtr->note_length = lstrlenW(note);
            infoPtr->note = heap_strndupW(note, infoPtr->note_length);
        }

        if (!note || !infoPtr->note)
        {
            infoPtr->note_length = 0;
            infoPtr->note = heap_alloc_zero(sizeof(WCHAR));
        }

        SetLastError(NO_ERROR);
        return TRUE;
    }

    case BCM_GETNOTE:
    {
        DWORD *size = (DWORD *)wParam;
        WCHAR *buffer = (WCHAR *)lParam;
        INT length = 0;

        if (btn_type != BS_COMMANDLINK && btn_type != BS_DEFCOMMANDLINK)
        {
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }

        if (!buffer || !size || !infoPtr->note)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if (*size > 0)
        {
            length = min(*size - 1, infoPtr->note_length);
            memcpy(buffer, infoPtr->note, length * sizeof(WCHAR));
            buffer[length] = '\0';
        }

        if (*size < infoPtr->note_length + 1)
        {
            *size = infoPtr->note_length + 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        else
        {
            SetLastError(NO_ERROR);
            return TRUE;
        }
    }

    case BCM_GETNOTELENGTH:
    {
        if (btn_type != BS_COMMANDLINK && btn_type != BS_DEFCOMMANDLINK)
        {
            SetLastError(ERROR_NOT_SUPPORTED);
            return 0;
        }

        return infoPtr->note_length;
    }

    case WM_SETFONT:
        infoPtr->font = (HFONT)wParam;
        if (lParam) InvalidateRect(hWnd, NULL, TRUE);
        break;

    case WM_GETFONT:
        return (LRESULT)infoPtr->font;

    case WM_SETFOCUS:
        TRACE("WM_SETFOCUS %p\n",hWnd);
        infoPtr->state |= BST_FOCUS;
        paint_button( infoPtr, btn_type, ODA_FOCUS );
        if (style & BS_NOTIFY)
            BUTTON_NOTIFY_PARENT(hWnd, BN_SETFOCUS);
        break;

    case WM_KILLFOCUS:
        TRACE("WM_KILLFOCUS %p\n",hWnd);
        infoPtr->state &= ~BST_FOCUS;
        paint_button( infoPtr, btn_type, ODA_FOCUS );

        if ((infoPtr->state & BUTTON_BTNPRESSED) && GetCapture() == hWnd)
            ReleaseCapture();
        if (style & BS_NOTIFY)
            BUTTON_NOTIFY_PARENT(hWnd, BN_KILLFOCUS);

        InvalidateRect( hWnd, NULL, FALSE );
        break;

    case WM_SYSCOLORCHANGE:
        InvalidateRect( hWnd, NULL, FALSE );
        break;

    case BM_SETSTYLE:
        btn_type = wParam & BS_TYPEMASK;
        style = (style & ~BS_TYPEMASK) | btn_type;
        SetWindowLongW( hWnd, GWL_STYLE, style );

        /* Only redraw if lParam flag is set.*/
        if (lParam)
            InvalidateRect( hWnd, NULL, TRUE );

        break;

    case BM_CLICK:
	SendMessageW( hWnd, WM_LBUTTONDOWN, 0, 0 );
	SendMessageW( hWnd, WM_LBUTTONUP, 0, 0 );
	break;

    case BM_SETIMAGE:
        infoPtr->image_type = (DWORD)wParam;
        oldHbitmap = infoPtr->u.image;
        infoPtr->u.image = (HANDLE)lParam;
	InvalidateRect( hWnd, NULL, FALSE );
	return (LRESULT)oldHbitmap;

    case BM_GETIMAGE:
        return (LRESULT)infoPtr->u.image;

    case BCM_SETIMAGELIST:
    {
        BUTTON_IMAGELIST *imagelist = (BUTTON_IMAGELIST *)lParam;

        if (!imagelist) return FALSE;

        infoPtr->imagelist = *imagelist;
        return TRUE;
    }

    case BCM_GETIMAGELIST:
    {
        BUTTON_IMAGELIST *imagelist = (BUTTON_IMAGELIST *)lParam;

        if (!imagelist) return FALSE;

        *imagelist = infoPtr->imagelist;
        return TRUE;
    }

    case BM_GETCHECK:
        return infoPtr->state & 3;

    case BM_SETCHECK:
        if (wParam > maxCheckState[btn_type]) wParam = maxCheckState[btn_type];
        if ((btn_type == BS_RADIOBUTTON) || (btn_type == BS_AUTORADIOBUTTON))
        {
            style = wParam ? style | WS_TABSTOP : style & ~WS_TABSTOP;
            SetWindowLongW( hWnd, GWL_STYLE, style );
        }
        if ((infoPtr->state & 3) != wParam)
        {
            infoPtr->state = (infoPtr->state & ~3) | wParam;
            InvalidateRect( hWnd, NULL, FALSE );
        }
        if ((btn_type == BS_AUTORADIOBUTTON) && (wParam == BST_CHECKED) && (style & WS_CHILD))
            BUTTON_CheckAutoRadioButton( hWnd );
        break;

    case BM_GETSTATE:
        return infoPtr->state;

    case BM_SETSTATE:
        state = infoPtr->state;
        new_state = wParam ? BST_PUSHED : 0;

        if ((state ^ new_state) & BST_PUSHED)
        {
            if (wParam)
                state |= BST_PUSHED;
            else
                state &= ~BST_PUSHED;

            if (btn_type == BS_USERBUTTON)
                BUTTON_NOTIFY_PARENT( hWnd, (state & BST_PUSHED) ? BN_HILITE : BN_UNHILITE );
            infoPtr->state = state;

            InvalidateRect( hWnd, NULL, FALSE );
        }
        break;

    case BCM_SETTEXTMARGIN:
    {
        RECT *text_margin = (RECT *)lParam;

        if (!text_margin) return FALSE;

        infoPtr->text_margin = *text_margin;
        return TRUE;
    }

    case BCM_GETTEXTMARGIN:
    {
        RECT *text_margin = (RECT *)lParam;

        if (!text_margin) return FALSE;

        *text_margin = infoPtr->text_margin;
        return TRUE;
    }

    case WM_NCHITTEST:
        if(btn_type == BS_GROUPBOX) return HTTRANSPARENT;
        /* fall through */
    default:
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

/* If maxWidth is zero, rectangle width is unlimited */
static RECT BUTTON_GetTextRect(const BUTTON_INFO *infoPtr, HDC hdc, const WCHAR *text, LONG maxWidth)
{
    LONG style = GetWindowLongW(infoPtr->hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongW(infoPtr->hwnd, GWL_EXSTYLE);
    UINT dtStyle = BUTTON_BStoDT(style, exStyle);
    HFONT hPrevFont;
    RECT rect = {0};

    rect.right = maxWidth;
    hPrevFont = SelectObject(hdc, infoPtr->font);
    /* Calculate height without DT_VCENTER and DT_BOTTOM to get the correct height */
    DrawTextW(hdc, text, -1, &rect, (dtStyle & ~(DT_VCENTER | DT_BOTTOM)) | DT_CALCRECT);
    if (hPrevFont) SelectObject(hdc, hPrevFont);

    return rect;
}

static BOOL show_image_only(const BUTTON_INFO *infoPtr)
{
    LONG style = GetWindowLongW(infoPtr->hwnd, GWL_STYLE);
    return (style & (BS_ICON | BS_BITMAP)) && (infoPtr->u.image || infoPtr->imagelist.himl);
}

static BOOL show_image_and_text(const BUTTON_INFO *infoPtr)
{
    LONG style = GetWindowLongW(infoPtr->hwnd, GWL_STYLE);
    UINT type = get_button_type(style);
    return !(style & (BS_ICON | BS_BITMAP))
           && ((infoPtr->u.image
                && (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_USERBUTTON || type == BS_SPLITBUTTON
                    || type == BS_DEFSPLITBUTTON || type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK))
               || (infoPtr->imagelist.himl && type != BS_GROUPBOX));
}

static BOOL show_image(const BUTTON_INFO *infoPtr)
{
    return show_image_only(infoPtr) || show_image_and_text(infoPtr);
}

/* Get a bounding rectangle that is large enough to contain a image and a text side by side.
 * Note: (left,top) of the result rectangle may not be (0,0), offset it by yourself if needed */
static RECT BUTTON_GetBoundingLabelRect(LONG style, const RECT *textRect, const RECT *imageRect)
{
    RECT labelRect;
    RECT rect = *imageRect;
    INT textWidth = textRect->right - textRect->left;
    INT textHeight = textRect->bottom - textRect->top;
    INT imageWidth = imageRect->right - imageRect->left;
    INT imageHeight = imageRect->bottom - imageRect->top;

    if ((style & BS_CENTER) == BS_RIGHT)
        OffsetRect(&rect, textWidth, 0);
    else if ((style & BS_CENTER) == BS_LEFT)
        OffsetRect(&rect, -imageWidth, 0);
    else if ((style & BS_VCENTER) == BS_BOTTOM)
        OffsetRect(&rect, 0, textHeight);
    else if ((style & BS_VCENTER) == BS_TOP)
        OffsetRect(&rect, 0, -imageHeight);
    else
        OffsetRect(&rect, -imageWidth, 0);

    UnionRect(&labelRect, textRect, &rect);
    return labelRect;
}

/* Position a rectangle inside a bounding rectangle according to button alignment flags */
static void BUTTON_PositionRect(LONG style, const RECT *outerRect, RECT *innerRect, const RECT *margin)
{
    INT width = innerRect->right - innerRect->left;
    INT height = innerRect->bottom - innerRect->top;

    if ((style & WS_EX_RIGHT) && !(style & BS_CENTER)) style |= BS_CENTER;

    if (!(style & BS_CENTER))
    {
        /* Push button's text is centered by default, all other types have left aligned text */
        if (get_button_type(style) <= BS_DEFPUSHBUTTON)
            style |= BS_CENTER;
        else
            style |= BS_LEFT;
    }

    if (!(style & BS_VCENTER))
    {
        /* Group box's text is top aligned by default */
        if (get_button_type(style) == BS_GROUPBOX)
            style |= BS_TOP;
    }

    switch (style & BS_CENTER)
    {
    case BS_CENTER:
        innerRect->left = outerRect->left + (outerRect->right - outerRect->left - width) / 2;
        innerRect->right = innerRect->left + width;
        break;
    case BS_RIGHT:
        innerRect->right = outerRect->right - margin->right;
        innerRect->left = innerRect->right - width;
        break;
    case BS_LEFT:
    default:
        innerRect->left = outerRect->left + margin->left;
        innerRect->right = innerRect->left + width;
        break;
    }

    switch (style & BS_VCENTER)
    {
    case BS_TOP:
        innerRect->top = outerRect->top + margin->top;
        innerRect->bottom = innerRect->top + height;
        break;
    case BS_BOTTOM:
        innerRect->bottom = outerRect->bottom - margin->bottom;
        innerRect->top = innerRect->bottom - height;
        break;
    case BS_VCENTER:
    default:
        innerRect->top = outerRect->top + (outerRect->bottom - outerRect->top - height) / 2;
        innerRect->bottom = innerRect->top + height;
        break;
    }
}

/* Convert imagelist align style to button align style */
static UINT BUTTON_ILStoBS(UINT align)
{
    switch (align)
    {
    case BUTTON_IMAGELIST_ALIGN_TOP:
        return BS_CENTER | BS_TOP;
    case BUTTON_IMAGELIST_ALIGN_BOTTOM:
        return BS_CENTER | BS_BOTTOM;
    case BUTTON_IMAGELIST_ALIGN_CENTER:
        return BS_CENTER | BS_VCENTER;
    case BUTTON_IMAGELIST_ALIGN_RIGHT:
        return BS_RIGHT | BS_VCENTER;
    case BUTTON_IMAGELIST_ALIGN_LEFT:
    default:
        return BS_LEFT | BS_VCENTER;
    }
}

static SIZE BUTTON_GetImageSize(const BUTTON_INFO *infoPtr)
{
    ICONINFO iconInfo;
    BITMAP bm = {0};
    SIZE size = {0};

    /* ImageList has priority over image */
    if (infoPtr->imagelist.himl)
        ImageList_GetIconSize(infoPtr->imagelist.himl, &size.cx, &size.cy);
    else if (infoPtr->u.image)
    {
        if (infoPtr->image_type == IMAGE_ICON)
        {
            GetIconInfo(infoPtr->u.icon, &iconInfo);
            GetObjectW(iconInfo.hbmColor, sizeof(bm), &bm);
            DeleteObject(iconInfo.hbmColor);
            DeleteObject(iconInfo.hbmMask);
        }
        else if (infoPtr->image_type == IMAGE_BITMAP)
            GetObjectW(infoPtr->u.bitmap, sizeof(bm), &bm);

        size.cx = bm.bmWidth;
        size.cy = bm.bmHeight;
    }

    return size;
}

/**********************************************************************
 *       BUTTON_CalcLayoutRects
 *
 *   Calculates the rectangles of the button label(image and text) and its parts depending on a button's style.
 *
 * Returns flags to be passed to DrawText.
 * Calculated rectangle doesn't take into account button state
 * (pushed, etc.). If there is nothing to draw (no text/image) output
 * rectangle is empty, and return value is (UINT)-1.
 *
 * PARAMS:
 * infoPtr [I]   Button pointer
 * hdc     [I]   Handle to device context to draw to
 * labelRc [I/O] Input the rect the label to be positioned in, and output the label rect
 * imageRc [O]   Optional, output the image rect
 * textRc  [O]   Optional, output the text rect
 */
static UINT BUTTON_CalcLayoutRects(const BUTTON_INFO *infoPtr, HDC hdc, RECT *labelRc, RECT *imageRc, RECT *textRc)
{
   LONG style = GetWindowLongW( infoPtr->hwnd, GWL_STYLE );
   LONG ex_style = GetWindowLongW( infoPtr->hwnd, GWL_EXSTYLE );
   LONG split_style = infoPtr->imagelist.himl ? BUTTON_ILStoBS(infoPtr->imagelist.uAlign) : style;
   WCHAR *text = get_button_text(infoPtr);
   SIZE imageSize = BUTTON_GetImageSize(infoPtr);
   UINT dtStyle = BUTTON_BStoDT(style, ex_style);
   RECT labelRect, imageRect, imageRectWithMargin, textRect;
   LONG imageMarginWidth, imageMarginHeight;
   RECT emptyMargin = {0}, oneMargin = {1, 1, 1, 1};
   LONG maxTextWidth;

   /* Calculate label rectangle according to label type */
   if ((imageSize.cx == 0 && imageSize.cy == 0) && (text == NULL || text[0] == '\0'))
   {
       SetRectEmpty(labelRc);
       SetRectEmpty(imageRc);
       SetRectEmpty(textRc);
       heap_free(text);
       return (UINT)-1;
   }

   SetRect(&imageRect, 0, 0, imageSize.cx, imageSize.cy);
   imageRectWithMargin = imageRect;
   if (infoPtr->imagelist.himl)
   {
       imageRectWithMargin.top -= infoPtr->imagelist.margin.top;
       imageRectWithMargin.bottom += infoPtr->imagelist.margin.bottom;
       imageRectWithMargin.left -= infoPtr->imagelist.margin.left;
       imageRectWithMargin.right += infoPtr->imagelist.margin.right;
   }

   /* Show image only */
   if (show_image_only(infoPtr))
   {
       BUTTON_PositionRect(style, labelRc, &imageRect,
                           infoPtr->imagelist.himl ? &infoPtr->imagelist.margin : &emptyMargin);
       labelRect = imageRect;
       SetRectEmpty(&textRect);
   }
   else
   {
       /* Get text rect */
       maxTextWidth = labelRc->right - labelRc->left;
       textRect = BUTTON_GetTextRect(infoPtr, hdc, text, maxTextWidth);
       heap_free(text);

       /* Show image and text */
       if (show_image_and_text(infoPtr))
       {
           RECT boundingLabelRect, boundingImageRect, boundingTextRect;

           /* Get label rect */
           /* Image list may have different alignment than the button, use the whole rect for label in this case */
           if (infoPtr->imagelist.himl)
               labelRect = *labelRc;
           else
           {
               /* Get a label bounding rectangle to position the label in the user specified label rectangle because
                * text and image need to align together. */
               boundingLabelRect = BUTTON_GetBoundingLabelRect(split_style, &textRect, &imageRectWithMargin);
               BUTTON_PositionRect(split_style, labelRc, &boundingLabelRect, &emptyMargin);
               labelRect = boundingLabelRect;
           }

           /* When imagelist has center align, use the whole rect for imagelist and text */
           if(infoPtr->imagelist.himl && infoPtr->imagelist.uAlign == BUTTON_IMAGELIST_ALIGN_CENTER)
           {
               boundingImageRect = labelRect;
               boundingTextRect = labelRect;
               BUTTON_PositionRect(split_style, &boundingImageRect, &imageRect,
                                   infoPtr->imagelist.himl ? &infoPtr->imagelist.margin : &emptyMargin);
               /* Text doesn't use imagelist align */
               BUTTON_PositionRect(style, &boundingTextRect, &textRect, &oneMargin);
           }
           else
           {
               /* Get image rect */
               /* Split the label rect to two halves as two bounding rectangles for image and text */
               boundingImageRect = labelRect;
               imageMarginWidth = imageRectWithMargin.right - imageRectWithMargin.left;
               imageMarginHeight = imageRectWithMargin.bottom - imageRectWithMargin.top;
               if ((split_style & BS_CENTER) == BS_RIGHT)
                   boundingImageRect.left = boundingImageRect.right - imageMarginWidth;
               else if ((split_style & BS_CENTER) == BS_LEFT)
                   boundingImageRect.right = boundingImageRect.left + imageMarginWidth;
               else if ((split_style & BS_VCENTER) == BS_BOTTOM)
                   boundingImageRect.top = boundingImageRect.bottom - imageMarginHeight;
               else if ((split_style & BS_VCENTER) == BS_TOP)
                   boundingImageRect.bottom = boundingImageRect.top + imageMarginHeight;
               else
                   boundingImageRect.right = boundingImageRect.left + imageMarginWidth;
               BUTTON_PositionRect(split_style, &boundingImageRect, &imageRect,
                                   infoPtr->imagelist.himl ? &infoPtr->imagelist.margin : &emptyMargin);

               /* Get text rect */
               SubtractRect(&boundingTextRect, &labelRect, &boundingImageRect);
               /* Text doesn't use imagelist align */
               BUTTON_PositionRect(style, &boundingTextRect, &textRect, &oneMargin);
           }
       }
       /* Show text only */
       else
       {
           if (get_button_type(style) != BS_GROUPBOX)
               BUTTON_PositionRect(style, labelRc, &textRect, &oneMargin);
           else
               /* GroupBox is always top aligned */
               BUTTON_PositionRect((style & ~BS_VCENTER) | BS_TOP, labelRc, &textRect, &oneMargin);
           labelRect = textRect;
           SetRectEmpty(&imageRect);
       }
   }

   CopyRect(labelRc, &labelRect);
   CopyRect(imageRc, &imageRect);
   CopyRect(textRc, &textRect);

   return dtStyle;
}


/**********************************************************************
 *       BUTTON_DrawTextCallback
 *
 *   Callback function used by DrawStateW function.
 */
static BOOL CALLBACK BUTTON_DrawTextCallback(HDC hdc, LPARAM lp, WPARAM wp, int cx, int cy)
{
   RECT rc;

   SetRect(&rc, 0, 0, cx, cy);
   DrawTextW(hdc, (LPCWSTR)lp, -1, &rc, (UINT)wp);
   return TRUE;
}

/**********************************************************************
 *       BUTTON_DrawLabel
 *
 *   Common function for drawing button label.
 *
 * FIXME:
 *      1. When BS_SINGLELINE is specified and text contains '\t', '\n' or '\r' in the middle, they are rendered as
 *         squares now whereas they should be ignored.
 *      2. When BS_MULTILINE is specified and text contains space in the middle, the space mistakenly be rendered as newline.
 */
static void BUTTON_DrawLabel(const BUTTON_INFO *infoPtr, HDC hdc, UINT dtFlags, const RECT *imageRect,
                             const RECT *textRect)
{
   DRAWSTATEPROC lpOutputProc = NULL;
   LPARAM lp;
   WPARAM wp = 0;
   HBRUSH hbr = 0;
   UINT flags = IsWindowEnabled(infoPtr->hwnd) ? DSS_NORMAL : DSS_DISABLED;
   UINT imageFlags;
   LONG state = infoPtr->state;
   LONG draw_state;
   LONG style = GetWindowLongW( infoPtr->hwnd, GWL_STYLE );
   WCHAR *text = NULL;

   /* FIXME: To draw disabled label in Win31 look-and-feel, we probably
    * must use DSS_MONO flag and COLOR_GRAYTEXT brush (or maybe DSS_UNION).
    * I don't have Win31 on hand to verify that, so I leave it as is.
    */

   if ((style & BS_PUSHLIKE) && (state & BST_INDETERMINATE))
   {
      hbr = GetSysColorBrush(COLOR_GRAYTEXT);
      flags |= DSS_MONO;
   }

   if (show_image(infoPtr))
   {
       if (infoPtr->imagelist.himl)
       {
           if (ImageList_GetImageCount(infoPtr->imagelist.himl) == 1)
               ImageList_Draw(infoPtr->imagelist.himl, 0, hdc, imageRect->left, imageRect->top, ILD_NORMAL);
           else
           {
               draw_state = get_draw_state(infoPtr);
               ImageList_Draw(infoPtr->imagelist.himl, draw_state - 1, hdc, imageRect->left, imageRect->top,
                              ILD_NORMAL);
           }
       }
       else
       {
           switch (infoPtr->image_type)
           {
           case IMAGE_ICON:
               imageFlags = flags | DST_ICON;
               lp = (LPARAM)infoPtr->u.icon;
               break;
           case IMAGE_BITMAP:
               imageFlags = flags | DST_BITMAP;
               lp = (LPARAM)infoPtr->u.bitmap;
               break;
           default:
               return;
           }

           DrawStateW(hdc, hbr, lpOutputProc, lp, wp, imageRect->left, imageRect->top,
                      imageRect->right - imageRect->left, imageRect->bottom - imageRect->top, imageFlags);
       }
   }

   if (show_image_only(infoPtr)) return;

   /* DST_COMPLEX -- is 0 */
   lpOutputProc = BUTTON_DrawTextCallback;
   if (!(text = get_button_text(infoPtr))) return;
   lp = (LPARAM)text;
   wp = dtFlags;
   DrawStateW(hdc, hbr, lpOutputProc, lp, wp, textRect->left, textRect->top, textRect->right - textRect->left,
              textRect->bottom - textRect->top, flags);
   heap_free(text);
}

/**********************************************************************
 *       Push Button Functions
 */
static void PB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action )
{
    RECT     rc, labelRect, imageRect, textRect;
    UINT     dtFlags, uState;
    HPEN     hOldPen, hpen;
    HBRUSH   hOldBrush;
    INT      oldBkMode;
    COLORREF oldTxtColor;
    HFONT hFont;
    LONG state = infoPtr->state;
    LONG style = GetWindowLongW( infoPtr->hwnd, GWL_STYLE );
    BOOL pushedState = (state & BST_PUSHED);
    HWND parent;
    HRGN hrgn;

    GetClientRect( infoPtr->hwnd, &rc );

    /* Send WM_CTLCOLOR to allow changing the font (the colors are fixed) */
    if ((hFont = infoPtr->font)) SelectObject( hDC, hFont );
    parent = GetParent(infoPtr->hwnd);
    if (!parent) parent = infoPtr->hwnd;
    SendMessageW( parent, WM_CTLCOLORBTN, (WPARAM)hDC, (LPARAM)infoPtr->hwnd );

    hrgn = set_control_clipping( hDC, &rc );

    hpen = CreatePen( PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
    hOldPen = SelectObject(hDC, hpen);
    hOldBrush = SelectObject(hDC,GetSysColorBrush(COLOR_BTNFACE));
    oldBkMode = SetBkMode(hDC, TRANSPARENT);

    if (get_button_type(style) == BS_DEFPUSHBUTTON)
    {
        if (action != ODA_FOCUS)
            Rectangle(hDC, rc.left, rc.top, rc.right, rc.bottom);
	InflateRect( &rc, -1, -1 );
    }

    /* completely skip the drawing if only focus has changed */
    if (action == ODA_FOCUS) goto draw_focus;

    uState = DFCS_BUTTONPUSH;

    if (style & BS_FLAT)
        uState |= DFCS_MONO;
    else if (pushedState)
    {
	if (get_button_type(style) == BS_DEFPUSHBUTTON )
	    uState |= DFCS_FLAT;
	else
	    uState |= DFCS_PUSHED;
    }

    if (state & (BST_CHECKED | BST_INDETERMINATE))
        uState |= DFCS_CHECKED;

    DrawFrameControl( hDC, &rc, DFC_BUTTON, uState );

    /* draw button label */
    labelRect = rc;
    /* Shrink label rect at all sides by 2 so that the content won't touch the surrounding frame */
    InflateRect(&labelRect, -2, -2);
    dtFlags = BUTTON_CalcLayoutRects(infoPtr, hDC, &labelRect, &imageRect, &textRect);

    if (dtFlags == (UINT)-1L)
       goto cleanup;

    if (pushedState) OffsetRect(&labelRect, 1, 1);

    oldTxtColor = SetTextColor( hDC, GetSysColor(COLOR_BTNTEXT) );

    BUTTON_DrawLabel(infoPtr, hDC, dtFlags, &imageRect, &textRect);

    SetTextColor( hDC, oldTxtColor );

draw_focus:
    if (action == ODA_FOCUS || (state & BST_FOCUS))
    {
        InflateRect( &rc, -2, -2 );
        DrawFocusRect( hDC, &rc );
    }

 cleanup:
    SelectObject( hDC, hOldPen );
    SelectObject( hDC, hOldBrush );
    SetBkMode(hDC, oldBkMode);
    SelectClipRgn( hDC, hrgn );
    if (hrgn) DeleteObject( hrgn );
    DeleteObject( hpen );
}

/**********************************************************************
 *       Check Box & Radio Button Functions
 */

static void CB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action )
{
    RECT rbox, labelRect, imageRect, textRect, client;
    HBRUSH hBrush;
    int delta, text_offset, checkBoxWidth, checkBoxHeight;
    UINT dtFlags;
    HFONT hFont;
    LONG state = infoPtr->state;
    LONG style = GetWindowLongW( infoPtr->hwnd, GWL_STYLE );
    LONG ex_style = GetWindowLongW( infoPtr->hwnd, GWL_EXSTYLE );
    HWND parent;
    HRGN hrgn;

    if (style & BS_PUSHLIKE)
    {
        PB_Paint( infoPtr, hDC, action );
	return;
    }

    GetClientRect(infoPtr->hwnd, &client);
    rbox = labelRect = client;

    checkBoxWidth  = 12 * GetDpiForWindow( infoPtr->hwnd ) / 96 + 1;
    checkBoxHeight = 12 * GetDpiForWindow( infoPtr->hwnd ) / 96 + 1;

    if ((hFont = infoPtr->font)) SelectObject( hDC, hFont );
    GetCharWidthW( hDC, '0', '0', &text_offset );
    text_offset /= 2;

    parent = GetParent(infoPtr->hwnd);
    if (!parent) parent = infoPtr->hwnd;
    hBrush = (HBRUSH)SendMessageW(parent, WM_CTLCOLORSTATIC, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);
    if (!hBrush) /* did the app forget to call defwindowproc ? */
        hBrush = (HBRUSH)DefWindowProcW(parent, WM_CTLCOLORSTATIC, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);
    hrgn = set_control_clipping( hDC, &client );

    if (style & BS_LEFTTEXT || ex_style & WS_EX_RIGHT)
    {
        labelRect.right -= checkBoxWidth + text_offset;
        rbox.left = rbox.right - checkBoxWidth;
    }
    else
    {
        labelRect.left += checkBoxWidth + text_offset;
        rbox.right = checkBoxWidth;
    }

    /* Since WM_ERASEBKGND does nothing, first prepare background */
    if (action == ODA_SELECT) FillRect( hDC, &rbox, hBrush );
    if (action == ODA_DRAWENTIRE) FillRect( hDC, &client, hBrush );

    /* Draw label */
    client = labelRect;
    dtFlags = BUTTON_CalcLayoutRects(infoPtr, hDC, &labelRect, &imageRect, &textRect);

    /* Only adjust rbox when rtext is valid */
    if (dtFlags != (UINT)-1L)
    {
        rbox.top = labelRect.top;
        rbox.bottom = labelRect.bottom;
    }

    /* Draw the check-box bitmap */
    if (action == ODA_DRAWENTIRE || action == ODA_SELECT)
    {
	UINT flags;

	if ((get_button_type(style) == BS_RADIOBUTTON) ||
	    (get_button_type(style) == BS_AUTORADIOBUTTON)) flags = DFCS_BUTTONRADIO;
	else if (state & BST_INDETERMINATE) flags = DFCS_BUTTON3STATE;
	else flags = DFCS_BUTTONCHECK;

	if (state & (BST_CHECKED | BST_INDETERMINATE)) flags |= DFCS_CHECKED;
	if (state & BST_PUSHED) flags |= DFCS_PUSHED;

	if (style & WS_DISABLED) flags |= DFCS_INACTIVE;

	/* rbox must have the correct height */
	delta = rbox.bottom - rbox.top - checkBoxHeight;

	if ((style & BS_VCENTER) == BS_TOP) {
	    if (delta > 0) {
		rbox.bottom = rbox.top + checkBoxHeight;
	    } else {
		rbox.top -= -delta/2 + 1;
		rbox.bottom = rbox.top + checkBoxHeight;
	    }
	} else if ((style & BS_VCENTER) == BS_BOTTOM) {
	    if (delta > 0) {
		rbox.top = rbox.bottom - checkBoxHeight;
	    } else {
		rbox.bottom += -delta/2 + 1;
		rbox.top = rbox.bottom - checkBoxHeight;
	    }
	} else { /* Default */
	    if (delta > 0) {
		int ofs = (delta / 2);
		rbox.bottom -= ofs + 1;
		rbox.top = rbox.bottom - checkBoxHeight;
	    } else if (delta < 0) {
		int ofs = (-delta / 2);
		rbox.top -= ofs + 1;
		rbox.bottom = rbox.top + checkBoxHeight;
	    }
	}

	DrawFrameControl( hDC, &rbox, DFC_BUTTON, flags );
    }

    if (dtFlags == (UINT)-1L) /* Noting to draw */
	return;

    if (action == ODA_DRAWENTIRE) BUTTON_DrawLabel(infoPtr, hDC, dtFlags, &imageRect, &textRect);

    /* ... and focus */
    if (action == ODA_FOCUS || (state & BST_FOCUS))
    {
        labelRect.left--;
        labelRect.right++;
        IntersectRect(&labelRect, &labelRect, &client);
        DrawFocusRect(hDC, &labelRect);
    }
    SelectClipRgn( hDC, hrgn );
    if (hrgn) DeleteObject( hrgn );
}


/**********************************************************************
 *       BUTTON_CheckAutoRadioButton
 *
 * hwnd is checked, uncheck every other auto radio button in group
 */
static void BUTTON_CheckAutoRadioButton( HWND hwnd )
{
    HWND parent, sibling, start;

    parent = GetParent(hwnd);
    /* make sure that starting control is not disabled or invisible */
    start = sibling = GetNextDlgGroupItem( parent, hwnd, TRUE );
    do
    {
        if (!sibling) break;
        if ((hwnd != sibling) &&
            ((GetWindowLongW( sibling, GWL_STYLE) & BS_TYPEMASK) == BS_AUTORADIOBUTTON))
            SendMessageW( sibling, BM_SETCHECK, BST_UNCHECKED, 0 );
        sibling = GetNextDlgGroupItem( parent, sibling, FALSE );
    } while (sibling != start);
}


/**********************************************************************
 *       Group Box Functions
 */

static void GB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action )
{
    RECT labelRect, imageRect, textRect, rcFrame;
    HBRUSH hbr;
    HFONT hFont;
    UINT dtFlags;
    TEXTMETRICW tm;
    LONG style = GetWindowLongW( infoPtr->hwnd, GWL_STYLE );
    HWND parent;
    HRGN hrgn;

    if ((hFont = infoPtr->font)) SelectObject( hDC, hFont );
    /* GroupBox acts like static control, so it sends CTLCOLORSTATIC */
    parent = GetParent(infoPtr->hwnd);
    if (!parent) parent = infoPtr->hwnd;
    hbr = (HBRUSH)SendMessageW(parent, WM_CTLCOLORSTATIC, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);
    if (!hbr) /* did the app forget to call defwindowproc ? */
        hbr = (HBRUSH)DefWindowProcW(parent, WM_CTLCOLORSTATIC, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);
    GetClientRect(infoPtr->hwnd, &labelRect);
    rcFrame = labelRect;
    hrgn = set_control_clipping(hDC, &labelRect);

    GetTextMetricsW (hDC, &tm);
    rcFrame.top += (tm.tmHeight / 2) - 1;
    DrawEdge (hDC, &rcFrame, EDGE_ETCHED, BF_RECT | ((style & BS_FLAT) ? BF_FLAT : 0));

    InflateRect(&labelRect, -7, 1);
    dtFlags = BUTTON_CalcLayoutRects(infoPtr, hDC, &labelRect, &imageRect, &textRect);

    if (dtFlags != (UINT)-1)
    {
        /* Because buttons have CS_PARENTDC class style, there is a chance
         * that label will be drawn out of client rect.
         * But Windows doesn't clip label's rect, so do I.
         */

        /* There is 1-pixel margin at the left, right, and bottom */
        labelRect.left--;
        labelRect.right++;
        labelRect.bottom++;
        FillRect(hDC, &labelRect, hbr);
        labelRect.left++;
        labelRect.right--;
        labelRect.bottom--;

        BUTTON_DrawLabel(infoPtr, hDC, dtFlags, &imageRect, &textRect);
    }
    SelectClipRgn( hDC, hrgn );
    if (hrgn) DeleteObject( hrgn );
}


/**********************************************************************
 *       User Button Functions
 */

static void UB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action )
{
    RECT rc;
    HBRUSH hBrush;
    HFONT hFont;
    LONG state = infoPtr->state;
    HWND parent;

    GetClientRect( infoPtr->hwnd, &rc);

    if ((hFont = infoPtr->font)) SelectObject( hDC, hFont );

    parent = GetParent(infoPtr->hwnd);
    if (!parent) parent = infoPtr->hwnd;
    hBrush = (HBRUSH)SendMessageW(parent, WM_CTLCOLORBTN, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);
    if (!hBrush) /* did the app forget to call defwindowproc ? */
        hBrush = (HBRUSH)DefWindowProcW(parent, WM_CTLCOLORBTN, (WPARAM)hDC, (LPARAM)infoPtr->hwnd);

    FillRect( hDC, &rc, hBrush );
    if (action == ODA_FOCUS || (state & BST_FOCUS))
        DrawFocusRect( hDC, &rc );

    switch (action)
    {
    case ODA_FOCUS:
        BUTTON_NOTIFY_PARENT( infoPtr->hwnd, (state & BST_FOCUS) ? BN_SETFOCUS : BN_KILLFOCUS );
        break;

    case ODA_SELECT:
        BUTTON_NOTIFY_PARENT( infoPtr->hwnd, (state & BST_PUSHED) ? BN_HILITE : BN_UNHILITE );
        break;

    default:
        break;
    }
}


/**********************************************************************
 *       Ownerdrawn Button Functions
 */

static void OB_Paint( const BUTTON_INFO *infoPtr, HDC hDC, UINT action )
{
    LONG state = infoPtr->state;
    DRAWITEMSTRUCT dis;
    LONG_PTR id = GetWindowLongPtrW( infoPtr->hwnd, GWLP_ID );
    HWND parent;
    HFONT hFont;
    HRGN hrgn;

    dis.CtlType    = ODT_BUTTON;
    dis.CtlID      = id;
    dis.itemID     = 0;
    dis.itemAction = action;
    dis.itemState  = ((state & BST_FOCUS) ? ODS_FOCUS : 0) |
                     ((state & BST_PUSHED) ? ODS_SELECTED : 0) |
                     (IsWindowEnabled(infoPtr->hwnd) ? 0: ODS_DISABLED);
    dis.hwndItem   = infoPtr->hwnd;
    dis.hDC        = hDC;
    dis.itemData   = 0;
    GetClientRect( infoPtr->hwnd, &dis.rcItem );

    if ((hFont = infoPtr->font)) SelectObject( hDC, hFont );
    parent = GetParent(infoPtr->hwnd);
    if (!parent) parent = infoPtr->hwnd;
    SendMessageW( parent, WM_CTLCOLORBTN, (WPARAM)hDC, (LPARAM)infoPtr->hwnd );

    hrgn = set_control_clipping( hDC, &dis.rcItem );

    SendMessageW( GetParent(infoPtr->hwnd), WM_DRAWITEM, id, (LPARAM)&dis );
    SelectClipRgn( hDC, hrgn );
    if (hrgn) DeleteObject( hrgn );
}

static void PB_ThemedPaint(HTHEME theme, const BUTTON_INFO *infoPtr, HDC hDC, int state, UINT dtFlags, BOOL focused)
{
    RECT bgRect, textRect;
    HFONT font = infoPtr->font;
    HFONT hPrevFont = font ? SelectObject(hDC, font) : NULL;
    WCHAR *text = get_button_text(infoPtr);

    GetClientRect(infoPtr->hwnd, &bgRect);
    GetThemeBackgroundContentRect(theme, hDC, BP_PUSHBUTTON, state, &bgRect, &textRect);

    if (IsThemeBackgroundPartiallyTransparent(theme, BP_PUSHBUTTON, state))
        DrawThemeParentBackground(infoPtr->hwnd, hDC, NULL);
    DrawThemeBackground(theme, hDC, BP_PUSHBUTTON, state, &bgRect, NULL);
    if (text)
    {
        DrawThemeText(theme, hDC, BP_PUSHBUTTON, state, text, lstrlenW(text), dtFlags, 0, &textRect);
        heap_free(text);
    }

    if (focused)
    {
        MARGINS margins;
        RECT focusRect = bgRect;

        GetThemeMargins(theme, hDC, BP_PUSHBUTTON, state, TMT_CONTENTMARGINS, NULL, &margins);

        focusRect.left += margins.cxLeftWidth;
        focusRect.top += margins.cyTopHeight;
        focusRect.right -= margins.cxRightWidth;
        focusRect.bottom -= margins.cyBottomHeight;

        DrawFocusRect( hDC, &focusRect );
    }

    if (hPrevFont) SelectObject(hDC, hPrevFont);
}

static void CB_ThemedPaint(HTHEME theme, const BUTTON_INFO *infoPtr, HDC hDC, int state, UINT dtFlags, BOOL focused)
{
    SIZE sz;
    RECT bgRect, textRect;
    HFONT font, hPrevFont = NULL;
    DWORD dwStyle = GetWindowLongW(infoPtr->hwnd, GWL_STYLE);
    UINT btn_type = get_button_type( dwStyle );
    int part = (btn_type == BS_RADIOBUTTON) || (btn_type == BS_AUTORADIOBUTTON) ? BP_RADIOBUTTON : BP_CHECKBOX;
    WCHAR *text = get_button_text(infoPtr);
    LOGFONTW lf;
    BOOL created_font = FALSE;

    HRESULT hr = GetThemeFont(theme, hDC, part, state, TMT_FONT, &lf);
    if (SUCCEEDED(hr)) {
        font = CreateFontIndirectW(&lf);
        if (!font)
            TRACE("Failed to create font\n");
        else {
            TRACE("font = %s\n", debugstr_w(lf.lfFaceName));
            hPrevFont = SelectObject(hDC, font);
            created_font = TRUE;
        }
    } else {
        font = (HFONT)SendMessageW(infoPtr->hwnd, WM_GETFONT, 0, 0);
        hPrevFont = SelectObject(hDC, font);
    }

    if (FAILED(GetThemePartSize(theme, hDC, part, state, NULL, TS_DRAW, &sz)))
        sz.cx = sz.cy = 13;

    GetClientRect(infoPtr->hwnd, &bgRect);
    GetThemeBackgroundContentRect(theme, hDC, part, state, &bgRect, &textRect);

    if (dtFlags & DT_SINGLELINE) /* Center the checkbox / radio button to the text. */
        bgRect.top = bgRect.top + (textRect.bottom - textRect.top - sz.cy) / 2;

    /* adjust for the check/radio marker */
    bgRect.bottom = bgRect.top + sz.cy;
    bgRect.right = bgRect.left + sz.cx;
    textRect.left = bgRect.right + 6;

    DrawThemeParentBackground(infoPtr->hwnd, hDC, NULL);

    DrawThemeBackground(theme, hDC, part, state, &bgRect, NULL);
    if (text)
    {
        DrawThemeText(theme, hDC, part, state, text, lstrlenW(text), dtFlags, 0, &textRect);

        if (focused)
        {
            RECT focusRect;

            focusRect = textRect;

            DrawTextW(hDC, text, lstrlenW(text), &focusRect, dtFlags | DT_CALCRECT);

            if (focusRect.right < textRect.right) focusRect.right++;
            focusRect.bottom = textRect.bottom;

            DrawFocusRect( hDC, &focusRect );
        }

        heap_free(text);
    }

    if (created_font) DeleteObject(font);
    if (hPrevFont) SelectObject(hDC, hPrevFont);
}

static void GB_ThemedPaint(HTHEME theme, const BUTTON_INFO *infoPtr, HDC hDC, int state, UINT dtFlags, BOOL focused)
{
    RECT bgRect, textRect, contentRect;
    WCHAR *text = get_button_text(infoPtr);
    LOGFONTW lf;
    HFONT font, hPrevFont = NULL;
    BOOL created_font = FALSE;

    HRESULT hr = GetThemeFont(theme, hDC, BP_GROUPBOX, state, TMT_FONT, &lf);
    if (SUCCEEDED(hr)) {
        font = CreateFontIndirectW(&lf);
        if (!font)
            TRACE("Failed to create font\n");
        else {
            hPrevFont = SelectObject(hDC, font);
            created_font = TRUE;
        }
    } else {
        font = (HFONT)SendMessageW(infoPtr->hwnd, WM_GETFONT, 0, 0);
        hPrevFont = SelectObject(hDC, font);
    }

    GetClientRect(infoPtr->hwnd, &bgRect);
    textRect = bgRect;

    if (text)
    {
        SIZE textExtent;
        GetTextExtentPoint32W(hDC, text, lstrlenW(text), &textExtent);
        bgRect.top += (textExtent.cy / 2);
        textRect.left += 10;
        textRect.bottom = textRect.top + textExtent.cy;
        textRect.right = textRect.left + textExtent.cx + 4;

        ExcludeClipRect(hDC, textRect.left, textRect.top, textRect.right, textRect.bottom);
    }

    GetThemeBackgroundContentRect(theme, hDC, BP_GROUPBOX, state, &bgRect, &contentRect);
    ExcludeClipRect(hDC, contentRect.left, contentRect.top, contentRect.right, contentRect.bottom);

    if (IsThemeBackgroundPartiallyTransparent(theme, BP_GROUPBOX, state))
        DrawThemeParentBackground(infoPtr->hwnd, hDC, NULL);
    DrawThemeBackground(theme, hDC, BP_GROUPBOX, state, &bgRect, NULL);

    SelectClipRgn(hDC, NULL);

    if (text)
    {
        InflateRect(&textRect, -2, 0);
        DrawThemeText(theme, hDC, BP_GROUPBOX, state, text, lstrlenW(text), 0, 0, &textRect);
        heap_free(text);
    }

    if (created_font) DeleteObject(font);
    if (hPrevFont) SelectObject(hDC, hPrevFont);
}

void BUTTON_Register(void)
{
    WNDCLASSW wndClass;

    memset(&wndClass, 0, sizeof(wndClass));
    wndClass.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC;
    wndClass.lpfnWndProc = BUTTON_WindowProc;
    wndClass.cbClsExtra = 0;
    wndClass.cbWndExtra = sizeof(BUTTON_INFO *);
    wndClass.hCursor = LoadCursorW(0, (LPWSTR)IDC_ARROW);
    wndClass.hbrBackground = NULL;
    wndClass.lpszClassName = WC_BUTTONW;
    RegisterClassW(&wndClass);
}
