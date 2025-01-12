/*
 * Window messaging support
 *
 * Copyright 2001 Alexandre Julliard
 * Copyright 2008 Maarten Lankhorst
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
 */

#include <assert.h>
#include <stdarg.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winerror.h"
#include "winnls.h"
#include "dbt.h"
#include "dde.h"
#include "imm.h"
#include "hidusage.h"
#include "ddk/imm.h"
#include "wine/server.h"
#include "user_private.h"
#include "win.h"
#include "controls.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(msg);
WINE_DECLARE_DEBUG_CHANNEL(key);

#define WM_NCMOUSEFIRST WM_NCMOUSEMOVE
#define WM_NCMOUSELAST  (WM_NCMOUSEFIRST+(WM_MOUSELAST-WM_MOUSEFIRST))

#define MAX_PACK_COUNT 4

#define MAX_WINPROC_RECURSION  64

struct packed_hook_extra_info
{
    user_handle_t handle;
    DWORD         __pad;
    ULONGLONG     lparam;
};

/* the structures are unpacked on top of the packed ones, so make sure they fit */
C_ASSERT( sizeof(struct packed_CREATESTRUCTW) >= sizeof(CREATESTRUCTW) );
C_ASSERT( sizeof(struct packed_DRAWITEMSTRUCT) >= sizeof(DRAWITEMSTRUCT) );
C_ASSERT( sizeof(struct packed_MEASUREITEMSTRUCT) >= sizeof(MEASUREITEMSTRUCT) );
C_ASSERT( sizeof(struct packed_DELETEITEMSTRUCT) >= sizeof(DELETEITEMSTRUCT) );
C_ASSERT( sizeof(struct packed_COMPAREITEMSTRUCT) >= sizeof(COMPAREITEMSTRUCT) );
C_ASSERT( sizeof(struct packed_WINDOWPOS) >= sizeof(WINDOWPOS) );
C_ASSERT( sizeof(struct packed_COPYDATASTRUCT) >= sizeof(COPYDATASTRUCT) );
C_ASSERT( sizeof(struct packed_HELPINFO) >= sizeof(HELPINFO) );
C_ASSERT( sizeof(struct packed_NCCALCSIZE_PARAMS) >= sizeof(NCCALCSIZE_PARAMS) + sizeof(WINDOWPOS) );
C_ASSERT( sizeof(struct packed_MSG) >= sizeof(MSG) );
C_ASSERT( sizeof(struct packed_MDINEXTMENU) >= sizeof(MDINEXTMENU) );
C_ASSERT( sizeof(struct packed_MDICREATESTRUCTW) >= sizeof(MDICREATESTRUCTW) );
C_ASSERT( sizeof(struct packed_hook_extra_info) >= sizeof(struct hook_extra_info) );

union packed_structs
{
    struct packed_CREATESTRUCTW cs;
    struct packed_DRAWITEMSTRUCT dis;
    struct packed_MEASUREITEMSTRUCT mis;
    struct packed_DELETEITEMSTRUCT dls;
    struct packed_COMPAREITEMSTRUCT cis;
    struct packed_WINDOWPOS wp;
    struct packed_COPYDATASTRUCT cds;
    struct packed_HELPINFO hi;
    struct packed_NCCALCSIZE_PARAMS ncp;
    struct packed_MSG msg;
    struct packed_MDINEXTMENU mnm;
    struct packed_MDICREATESTRUCTW mcs;
    struct packed_hook_extra_info hook;
};

/* description of the data fields that need to be packed along with a sent message */
struct packed_message
{
    union packed_structs ps;
    int                  count;
    const void          *data[MAX_PACK_COUNT];
    size_t               size[MAX_PACK_COUNT];
};

/* structure to group all parameters for sent messages of the various kinds */
struct send_message_info
{
    enum message_type type;
    DWORD             dest_tid;
    HWND              hwnd;
    UINT              msg;
    WPARAM            wparam;
    LPARAM            lparam;
    UINT              flags;      /* flags for SendMessageTimeout */
    UINT              timeout;    /* timeout for SendMessageTimeout */
    SENDASYNCPROC     callback;   /* callback function for SendMessageCallback */
    ULONG_PTR         data;       /* callback data */
    enum wm_char_mapping wm_char;
};

/* Message class descriptor */
const struct builtin_class_descr MESSAGE_builtin_class =
{
    L"Message",           /* name */
    0,                    /* style */
    WINPROC_MESSAGE,      /* proc */
    0,                    /* extra */
    0,                    /* cursor */
    0                     /* brush */
};



/* flag for messages that contain pointers */
/* 32 messages per entry, messages 0..31 map to bits 0..31 */

#define SET(msg) (1 << ((msg) & 31))

static const unsigned int message_pointer_flags[] =
{
    /* 0x00 - 0x1f */
    SET(WM_CREATE) | SET(WM_SETTEXT) | SET(WM_GETTEXT) |
    SET(WM_WININICHANGE) | SET(WM_DEVMODECHANGE),
    /* 0x20 - 0x3f */
    SET(WM_GETMINMAXINFO) | SET(WM_DRAWITEM) | SET(WM_MEASUREITEM) | SET(WM_DELETEITEM) |
    SET(WM_COMPAREITEM),
    /* 0x40 - 0x5f */
    SET(WM_WINDOWPOSCHANGING) | SET(WM_WINDOWPOSCHANGED) | SET(WM_COPYDATA) | SET(WM_HELP),
    /* 0x60 - 0x7f */
    SET(WM_STYLECHANGING) | SET(WM_STYLECHANGED),
    /* 0x80 - 0x9f */
    SET(WM_NCCREATE) | SET(WM_NCCALCSIZE) | SET(WM_GETDLGCODE),
    /* 0xa0 - 0xbf */
    SET(EM_GETSEL) | SET(EM_GETRECT) | SET(EM_SETRECT) | SET(EM_SETRECTNP),
    /* 0xc0 - 0xdf */
    SET(EM_REPLACESEL) | SET(EM_GETLINE) | SET(EM_SETTABSTOPS),
    /* 0xe0 - 0xff */
    SET(SBM_GETRANGE) | SET(SBM_SETSCROLLINFO) | SET(SBM_GETSCROLLINFO) | SET(SBM_GETSCROLLBARINFO),
    /* 0x100 - 0x11f */
    0,
    /* 0x120 - 0x13f */
    0,
    /* 0x140 - 0x15f */
    SET(CB_GETEDITSEL) | SET(CB_ADDSTRING) | SET(CB_DIR) | SET(CB_GETLBTEXT) |
    SET(CB_INSERTSTRING) | SET(CB_FINDSTRING) | SET(CB_SELECTSTRING) |
    SET(CB_GETDROPPEDCONTROLRECT) | SET(CB_FINDSTRINGEXACT),
    /* 0x160 - 0x17f */
    0,
    /* 0x180 - 0x19f */
    SET(LB_ADDSTRING) | SET(LB_INSERTSTRING) | SET(LB_GETTEXT) | SET(LB_SELECTSTRING) |
    SET(LB_DIR) | SET(LB_FINDSTRING) |
    SET(LB_GETSELITEMS) | SET(LB_SETTABSTOPS) | SET(LB_ADDFILE) | SET(LB_GETITEMRECT),
    /* 0x1a0 - 0x1bf */
    SET(LB_FINDSTRINGEXACT),
    /* 0x1c0 - 0x1df */
    0,
    /* 0x1e0 - 0x1ff */
    0,
    /* 0x200 - 0x21f */
    SET(WM_NEXTMENU) | SET(WM_SIZING) | SET(WM_MOVING) | SET(WM_DEVICECHANGE),
    /* 0x220 - 0x23f */
    SET(WM_MDICREATE) | SET(WM_MDIGETACTIVE) | SET(WM_DROPOBJECT) |
    SET(WM_QUERYDROPOBJECT) | SET(WM_DRAGLOOP) | SET(WM_DRAGSELECT) | SET(WM_DRAGMOVE),
    /* 0x240 - 0x25f */
    0,
    /* 0x260 - 0x27f */
    0,
    /* 0x280 - 0x29f */
    0,
    /* 0x2a0 - 0x2bf */
    0,
    /* 0x2c0 - 0x2df */
    0,
    /* 0x2e0 - 0x2ff */
    0,
    /* 0x300 - 0x31f */
    SET(WM_ASKCBFORMATNAME)
};

/* check whether a given message type includes pointers */
static inline BOOL is_pointer_message( UINT message, WPARAM wparam )
{
    if (message >= 8*sizeof(message_pointer_flags)) return FALSE;
    if (message == WM_DEVICECHANGE && !(wparam & 0x8000)) return FALSE;
    return (message_pointer_flags[message / 32] & SET(message)) != 0;
}

#undef SET

/* add a data field to a packed message */
static inline void push_data( struct packed_message *data, const void *ptr, size_t size )
{
    data->data[data->count] = ptr;
    data->size[data->count] = size;
    data->count++;
}

/* add a string to a packed message */
static inline void push_string( struct packed_message *data, LPCWSTR str )
{
    push_data( data, str, (lstrlenW(str) + 1) * sizeof(WCHAR) );
}

/* pack a pointer into a 32/64 portable format */
static inline ULONGLONG pack_ptr( const void *ptr )
{
    return (ULONG_PTR)ptr;
}

/* unpack a potentially 64-bit pointer, returning 0 when truncated */
static inline void *unpack_ptr( ULONGLONG ptr64 )
{
    if ((ULONG_PTR)ptr64 != ptr64) return 0;
    return (void *)(ULONG_PTR)ptr64;
}

/* check whether a combobox expects strings or ids in CB_ADDSTRING/CB_INSERTSTRING */
static inline BOOL combobox_has_strings( HWND hwnd )
{
    DWORD style = GetWindowLongA( hwnd, GWL_STYLE );
    return (!(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (style & CBS_HASSTRINGS));
}

/* check whether a listbox expects strings or ids in LB_ADDSTRING/LB_INSERTSTRING */
static inline BOOL listbox_has_strings( HWND hwnd )
{
    DWORD style = GetWindowLongA( hwnd, GWL_STYLE );
    return (!(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (style & LBS_HASSTRINGS));
}

/* check whether message is in the range of keyboard messages */
static inline BOOL is_keyboard_message( UINT message )
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST);
}

/* check whether message is in the range of mouse messages */
static inline BOOL is_mouse_message( UINT message )
{
    return ((message >= WM_NCMOUSEFIRST && message <= WM_NCMOUSELAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST));
}

/* check whether message matches the specified hwnd filter */
static inline BOOL check_hwnd_filter( const MSG *msg, HWND hwnd_filter )
{
    if (!hwnd_filter || hwnd_filter == GetDesktopWindow()) return TRUE;
    return (msg->hwnd == hwnd_filter || IsChild( hwnd_filter, msg->hwnd ));
}

/* check for pending WM_CHAR message with DBCS trailing byte */
static inline BOOL get_pending_wmchar( MSG *msg, UINT first, UINT last, BOOL remove )
{
    struct wm_char_mapping_data *data = get_user_thread_info()->wmchar_data;

    if (!data || !data->get_msg.message) return FALSE;
    if ((first || last) && (first > WM_CHAR || last < WM_CHAR)) return FALSE;
    if (!msg) return FALSE;
    *msg = data->get_msg;
    if (remove) data->get_msg.message = 0;
    return TRUE;
}


/***********************************************************************
 *           MessageWndProc
 *
 * Window procedure for "Message" windows (HWND_MESSAGE parent).
 */
LRESULT WINAPI MessageWndProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    if (message == WM_NCCREATE) return TRUE;
    return 0;  /* all other messages are ignored */
}


/***********************************************************************
 *		broadcast_message_callback
 *
 * Helper callback for broadcasting messages.
 */
static BOOL CALLBACK broadcast_message_callback( HWND hwnd, LPARAM lparam )
{
    struct send_message_info *info = (struct send_message_info *)lparam;
    if ((GetWindowLongW( hwnd, GWL_STYLE ) & (WS_POPUP|WS_CHILD)) == WS_CHILD)
        return TRUE;
    switch(info->type)
    {
    case MSG_UNICODE:
        SendMessageTimeoutW( hwnd, info->msg, info->wparam, info->lparam,
                             info->flags, info->timeout, NULL );
        break;
    case MSG_ASCII:
        SendMessageTimeoutA( hwnd, info->msg, info->wparam, info->lparam,
                             info->flags, info->timeout, NULL );
        break;
    case MSG_NOTIFY:
        SendNotifyMessageW( hwnd, info->msg, info->wparam, info->lparam );
        break;
    case MSG_CALLBACK:
        SendMessageCallbackW( hwnd, info->msg, info->wparam, info->lparam,
                              info->callback, info->data );
        break;
    case MSG_POSTED:
        PostMessageW( hwnd, info->msg, info->wparam, info->lparam );
        break;
    default:
        ERR( "bad type %d\n", info->type );
        break;
    }
    return TRUE;
}

DWORD get_input_codepage( void )
{
    DWORD cp;
    int ret;
    HKL hkl = NtUserGetKeyboardLayout( 0 );

    ret = GetLocaleInfoW( LOWORD(hkl), LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                          (WCHAR *)&cp, sizeof(cp) / sizeof(WCHAR) );
    if (!ret) cp = CP_ACP;
    return cp;
}

/***********************************************************************
 *		map_wparam_AtoW
 *
 * Convert the wparam of an ASCII message to Unicode.
 */
BOOL map_wparam_AtoW( UINT message, WPARAM *wparam, enum wm_char_mapping mapping )
{
    char ch[2];
    WCHAR wch[2];
    DWORD cp;

    wch[0] = wch[1] = 0;
    switch(message)
    {
    case WM_CHAR:
        /* WM_CHAR is magic: a DBCS char can be sent/posted as two consecutive WM_CHAR
         * messages, in which case the first char is stored, and the conversion
         * to Unicode only takes place once the second char is sent/posted.
         */
        if (mapping != WMCHAR_MAP_NOMAPPING)
        {
            struct wm_char_mapping_data *data = get_user_thread_info()->wmchar_data;
            BYTE low = LOBYTE(*wparam);
            cp = get_input_codepage();

            if (HIBYTE(*wparam))
            {
                ch[0] = low;
                ch[1] = HIBYTE(*wparam);
                MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
                TRACE( "map %02x,%02x -> %04x mapping %u\n", (BYTE)ch[0], (BYTE)ch[1], wch[0], mapping );
                if (data) data->lead_byte[mapping] = 0;
            }
            else if (data && data->lead_byte[mapping])
            {
                ch[0] = data->lead_byte[mapping];
                ch[1] = low;
                MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
                TRACE( "map stored %02x,%02x -> %04x mapping %u\n", (BYTE)ch[0], (BYTE)ch[1], wch[0], mapping );
                data->lead_byte[mapping] = 0;
            }
            else if (!IsDBCSLeadByte( low ))
            {
                ch[0] = low;
                MultiByteToWideChar( cp, 0, ch, 1, wch, 2 );
                TRACE( "map %02x -> %04x\n", (BYTE)ch[0], wch[0] );
                if (data) data->lead_byte[mapping] = 0;
            }
            else  /* store it and wait for trail byte */
            {
                if (!data)
                {
                    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) )))
                        return FALSE;
                    get_user_thread_info()->wmchar_data = data;
                }
                TRACE( "storing lead byte %02x mapping %u\n", low, mapping );
                data->lead_byte[mapping] = low;
                return FALSE;
            }
            *wparam = MAKEWPARAM(wch[0], wch[1]);
            break;
        }
        /* else fall through */
    case WM_CHARTOITEM:
    case EM_SETPASSWORDCHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_MENUCHAR:
        cp = get_input_codepage();
        ch[0] = LOBYTE(*wparam);
        ch[1] = HIBYTE(*wparam);
        MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
        *wparam = MAKEWPARAM(wch[0], wch[1]);
        break;
    case WM_IME_CHAR:
        cp = get_input_codepage();
        ch[0] = HIBYTE(*wparam);
        ch[1] = LOBYTE(*wparam);
        if (ch[0]) MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
        else MultiByteToWideChar( cp, 0, ch + 1, 1, wch, 1 );
        *wparam = MAKEWPARAM(wch[0], HIWORD(*wparam));
        break;
    }
    return TRUE;
}


/***********************************************************************
 *		map_wparam_WtoA
 *
 * Convert the wparam of a Unicode message to ASCII.
 */
static void map_wparam_WtoA( MSG *msg, BOOL remove )
{
    BYTE ch[4];
    WCHAR wch[2];
    DWORD len;
    DWORD cp;

    switch(msg->message)
    {
    case WM_CHAR:
        if (!HIWORD(msg->wParam))
        {
            cp = get_input_codepage();
            wch[0] = LOWORD(msg->wParam);
            ch[0] = ch[1] = 0;
            len = WideCharToMultiByte( cp, 0, wch, 1, (LPSTR)ch, 2, NULL, NULL );
            if (len == 2)  /* DBCS char */
            {
                struct wm_char_mapping_data *data = get_user_thread_info()->wmchar_data;
                if (!data)
                {
                    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) ))) return;
                    get_user_thread_info()->wmchar_data = data;
                }
                if (remove)
                {
                    data->get_msg = *msg;
                    data->get_msg.wParam = ch[1];
                }
                msg->wParam = ch[0];
                return;
            }
        }
        /* else fall through */
    case WM_CHARTOITEM:
    case EM_SETPASSWORDCHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_MENUCHAR:
        cp = get_input_codepage();
        wch[0] = LOWORD(msg->wParam);
        wch[1] = HIWORD(msg->wParam);
        ch[0] = ch[1] = 0;
        WideCharToMultiByte( cp, 0, wch, 2, (LPSTR)ch, 4, NULL, NULL );
        msg->wParam = MAKEWPARAM( ch[0] | (ch[1] << 8), 0 );
        break;
    case WM_IME_CHAR:
        cp = get_input_codepage();
        wch[0] = LOWORD(msg->wParam);
        ch[0] = ch[1] = 0;
        len = WideCharToMultiByte( cp, 0, wch, 1, (LPSTR)ch, 2, NULL, NULL );
        if (len == 2)
            msg->wParam = MAKEWPARAM( (ch[0] << 8) | ch[1], HIWORD(msg->wParam) );
        else
            msg->wParam = MAKEWPARAM( ch[0], HIWORD(msg->wParam) );
        break;
    }
}


/***********************************************************************
 *		pack_message
 *
 * Pack a message for sending to another process.
 * Return the size of the data we expect in the message reply.
 * Set data->count to -1 if there is an error.
 */
static size_t pack_message( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                            struct packed_message *data )
{
    data->count = 0;
    switch(message)
    {
    case WM_NCCREATE:
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        data->ps.cs.lpCreateParams = pack_ptr( cs->lpCreateParams );
        data->ps.cs.hInstance      = pack_ptr( cs->hInstance );
        data->ps.cs.hMenu          = wine_server_user_handle( cs->hMenu );
        data->ps.cs.hwndParent     = wine_server_user_handle( cs->hwndParent );
        data->ps.cs.cy             = cs->cy;
        data->ps.cs.cx             = cs->cx;
        data->ps.cs.y              = cs->y;
        data->ps.cs.x              = cs->x;
        data->ps.cs.style          = cs->style;
        data->ps.cs.dwExStyle      = cs->dwExStyle;
        data->ps.cs.lpszName       = pack_ptr( cs->lpszName );
        data->ps.cs.lpszClass      = pack_ptr( cs->lpszClass );
        push_data( data, &data->ps.cs, sizeof(data->ps.cs) );
        if (!IS_INTRESOURCE(cs->lpszName)) push_string( data, cs->lpszName );
        if (!IS_INTRESOURCE(cs->lpszClass)) push_string( data, cs->lpszClass );
        return sizeof(data->ps.cs);
    }
    case WM_GETTEXT:
    case WM_ASKCBFORMATNAME:
        return wparam * sizeof(WCHAR);
    case WM_WININICHANGE:
        if (lparam) push_string(data, (LPWSTR)lparam );
        return 0;
    case WM_SETTEXT:
    case WM_DEVMODECHANGE:
    case CB_DIR:
    case LB_DIR:
    case LB_ADDFILE:
    case EM_REPLACESEL:
        push_string( data, (LPWSTR)lparam );
        return 0;
    case WM_GETMINMAXINFO:
        push_data( data, (MINMAXINFO *)lparam, sizeof(MINMAXINFO) );
        return sizeof(MINMAXINFO);
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lparam;
        data->ps.dis.CtlType    = dis->CtlType;
        data->ps.dis.CtlID      = dis->CtlID;
        data->ps.dis.itemID     = dis->itemID;
        data->ps.dis.itemAction = dis->itemAction;
        data->ps.dis.itemState  = dis->itemState;
        data->ps.dis.hwndItem   = wine_server_user_handle( dis->hwndItem );
        data->ps.dis.hDC        = wine_server_user_handle( dis->hDC );  /* FIXME */
        data->ps.dis.rcItem     = dis->rcItem;
        data->ps.dis.itemData   = dis->itemData;
        push_data( data, &data->ps.dis, sizeof(data->ps.dis) );
        return 0;
    }
    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lparam;
        data->ps.mis.CtlType    = mis->CtlType;
        data->ps.mis.CtlID      = mis->CtlID;
        data->ps.mis.itemID     = mis->itemID;
        data->ps.mis.itemWidth  = mis->itemWidth;
        data->ps.mis.itemHeight = mis->itemHeight;
        data->ps.mis.itemData   = mis->itemData;
        push_data( data, &data->ps.mis, sizeof(data->ps.mis) );
        return sizeof(data->ps.mis);
    }
    case WM_DELETEITEM:
    {
        DELETEITEMSTRUCT *dls = (DELETEITEMSTRUCT *)lparam;
        data->ps.dls.CtlType    = dls->CtlType;
        data->ps.dls.CtlID      = dls->CtlID;
        data->ps.dls.itemID     = dls->itemID;
        data->ps.dls.hwndItem   = wine_server_user_handle( dls->hwndItem );
        data->ps.dls.itemData   = dls->itemData;
        push_data( data, &data->ps.dls, sizeof(data->ps.dls) );
        return 0;
    }
    case WM_COMPAREITEM:
    {
        COMPAREITEMSTRUCT *cis = (COMPAREITEMSTRUCT *)lparam;
        data->ps.cis.CtlType    = cis->CtlType;
        data->ps.cis.CtlID      = cis->CtlID;
        data->ps.cis.hwndItem   = wine_server_user_handle( cis->hwndItem );
        data->ps.cis.itemID1    = cis->itemID1;
        data->ps.cis.itemData1  = cis->itemData1;
        data->ps.cis.itemID2    = cis->itemID2;
        data->ps.cis.itemData2  = cis->itemData2;
        data->ps.cis.dwLocaleId = cis->dwLocaleId;
        push_data( data, &data->ps.cis, sizeof(data->ps.cis) );
        return 0;
    }
    case WM_WINE_SETWINDOWPOS:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    {
        WINDOWPOS *wp = (WINDOWPOS *)lparam;
        data->ps.wp.hwnd            = wine_server_user_handle( wp->hwnd );
        data->ps.wp.hwndInsertAfter = wine_server_user_handle( wp->hwndInsertAfter );
        data->ps.wp.x               = wp->x;
        data->ps.wp.y               = wp->y;
        data->ps.wp.cx              = wp->cx;
        data->ps.wp.cy              = wp->cy;
        data->ps.wp.flags           = wp->flags;
        push_data( data, &data->ps.wp, sizeof(data->ps.wp) );
        return sizeof(data->ps.wp);
    }
    case WM_COPYDATA:
    {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lparam;
        data->ps.cds.cbData = cds->cbData;
        data->ps.cds.dwData = cds->dwData;
        data->ps.cds.lpData = pack_ptr( cds->lpData );
        push_data( data, &data->ps.cds, sizeof(data->ps.cds) );
        if (cds->lpData) push_data( data, cds->lpData, cds->cbData );
        return 0;
    }
    case WM_NOTIFY:
        /* WM_NOTIFY cannot be sent across processes (MSDN) */
        data->count = -1;
        return 0;
    case WM_HELP:
    {
        HELPINFO *hi = (HELPINFO *)lparam;
        data->ps.hi.iContextType = hi->iContextType;
        data->ps.hi.iCtrlId      = hi->iCtrlId;
        data->ps.hi.hItemHandle  = wine_server_user_handle( hi->hItemHandle );
        data->ps.hi.dwContextId  = hi->dwContextId;
        data->ps.hi.MousePos     = hi->MousePos;
        push_data( data, &data->ps.hi, sizeof(data->ps.hi) );
        return 0;
    }
    case WM_STYLECHANGING:
    case WM_STYLECHANGED:
        push_data( data, (STYLESTRUCT *)lparam, sizeof(STYLESTRUCT) );
        return 0;
    case WM_NCCALCSIZE:
        if (!wparam)
        {
            push_data( data, (RECT *)lparam, sizeof(RECT) );
            return sizeof(RECT);
        }
        else
        {
            NCCALCSIZE_PARAMS *ncp = (NCCALCSIZE_PARAMS *)lparam;
            data->ps.ncp.rgrc[0]         = ncp->rgrc[0];
            data->ps.ncp.rgrc[1]         = ncp->rgrc[1];
            data->ps.ncp.rgrc[2]         = ncp->rgrc[2];
            data->ps.ncp.hwnd            = wine_server_user_handle( ncp->lppos->hwnd );
            data->ps.ncp.hwndInsertAfter = wine_server_user_handle( ncp->lppos->hwndInsertAfter );
            data->ps.ncp.x               = ncp->lppos->x;
            data->ps.ncp.y               = ncp->lppos->y;
            data->ps.ncp.cx              = ncp->lppos->cx;
            data->ps.ncp.cy              = ncp->lppos->cy;
            data->ps.ncp.flags           = ncp->lppos->flags;
            push_data( data, &data->ps.ncp, sizeof(data->ps.ncp) );
            return sizeof(data->ps.ncp);
        }
    case WM_GETDLGCODE:
        if (lparam)
        {
            MSG *msg = (MSG *)lparam;
            data->ps.msg.hwnd    = wine_server_user_handle( msg->hwnd );
            data->ps.msg.message = msg->message;
            data->ps.msg.wParam  = msg->wParam;
            data->ps.msg.lParam  = msg->lParam;
            data->ps.msg.time    = msg->time;
            data->ps.msg.pt      = msg->pt;
            push_data( data, &data->ps.msg, sizeof(data->ps.msg) );
            return sizeof(data->ps.msg);
        }
        return 0;
    case SBM_SETSCROLLINFO:
        push_data( data, (SCROLLINFO *)lparam, sizeof(SCROLLINFO) );
        return 0;
    case SBM_GETSCROLLINFO:
        push_data( data, (SCROLLINFO *)lparam, sizeof(SCROLLINFO) );
        return sizeof(SCROLLINFO);
    case SBM_GETSCROLLBARINFO:
    {
        const SCROLLBARINFO *info = (const SCROLLBARINFO *)lparam;
        size_t size = min( info->cbSize, sizeof(SCROLLBARINFO) );
        push_data( data, info, size );
        return size;
    }
    case EM_GETSEL:
    case SBM_GETRANGE:
    case CB_GETEDITSEL:
    {
        size_t size = 0;
        if (wparam) size += sizeof(DWORD);
        if (lparam) size += sizeof(DWORD);
        return size;
    }
    case EM_GETRECT:
    case LB_GETITEMRECT:
    case CB_GETDROPPEDCONTROLRECT:
        return sizeof(RECT);
    case EM_SETRECT:
    case EM_SETRECTNP:
        push_data( data, (RECT *)lparam, sizeof(RECT) );
        return 0;
    case EM_GETLINE:
    {
        WORD *pw = (WORD *)lparam;
        push_data( data, pw, sizeof(*pw) );
        return *pw * sizeof(WCHAR);
    }
    case EM_SETTABSTOPS:
    case LB_SETTABSTOPS:
        if (wparam) push_data( data, (UINT *)lparam, sizeof(UINT) * wparam );
        return 0;
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_FINDSTRING:
    case CB_FINDSTRINGEXACT:
    case CB_SELECTSTRING:
        if (combobox_has_strings( hwnd )) push_string( data, (LPWSTR)lparam );
        return 0;
    case CB_GETLBTEXT:
        if (!combobox_has_strings( hwnd )) return sizeof(ULONG_PTR);
        return (SendMessageW( hwnd, CB_GETLBTEXTLEN, wparam, 0 ) + 1) * sizeof(WCHAR);
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING:
        if (listbox_has_strings( hwnd )) push_string( data, (LPWSTR)lparam );
        return 0;
    case LB_GETTEXT:
        if (!listbox_has_strings( hwnd )) return sizeof(ULONG_PTR);
        return (SendMessageW( hwnd, LB_GETTEXTLEN, wparam, 0 ) + 1) * sizeof(WCHAR);
    case LB_GETSELITEMS:
        return wparam * sizeof(UINT);
    case WM_NEXTMENU:
    {
        MDINEXTMENU *mnm = (MDINEXTMENU *)lparam;
        data->ps.mnm.hmenuIn   = wine_server_user_handle( mnm->hmenuIn );
        data->ps.mnm.hmenuNext = wine_server_user_handle( mnm->hmenuNext );
        data->ps.mnm.hwndNext  = wine_server_user_handle( mnm->hwndNext );
        push_data( data, &data->ps.mnm, sizeof(data->ps.mnm) );
        return sizeof(data->ps.mnm);
    }
    case WM_SIZING:
    case WM_MOVING:
        push_data( data, (RECT *)lparam, sizeof(RECT) );
        return sizeof(RECT);
    case WM_MDICREATE:
    {
        MDICREATESTRUCTW *mcs = (MDICREATESTRUCTW *)lparam;
        data->ps.mcs.szClass = pack_ptr( mcs->szClass );
        data->ps.mcs.szTitle = pack_ptr( mcs->szTitle );
        data->ps.mcs.hOwner  = pack_ptr( mcs->hOwner );
        data->ps.mcs.x       = mcs->x;
        data->ps.mcs.y       = mcs->y;
        data->ps.mcs.cx      = mcs->cx;
        data->ps.mcs.cy      = mcs->cy;
        data->ps.mcs.style   = mcs->style;
        data->ps.mcs.lParam  = mcs->lParam;
        push_data( data, &data->ps.mcs, sizeof(data->ps.mcs) );
        if (!IS_INTRESOURCE(mcs->szClass)) push_string( data, mcs->szClass );
        if (!IS_INTRESOURCE(mcs->szTitle)) push_string( data, mcs->szTitle );
        return sizeof(data->ps.mcs);
    }
    case WM_MDIGETACTIVE:
        if (lparam) return sizeof(BOOL);
        return 0;
    case WM_DEVICECHANGE:
    {
        DEV_BROADCAST_HDR *header = (DEV_BROADCAST_HDR *)lparam;
        if ((wparam & 0x8000) && header) push_data( data, header, header->dbch_size );
        return 0;
    }
    case WM_WINE_KEYBOARD_LL_HOOK:
    {
        struct hook_extra_info *h_extra = (struct hook_extra_info *)lparam;
        data->ps.hook.handle = wine_server_user_handle( h_extra->handle );
        push_data( data, &data->ps.hook, sizeof(data->ps.hook) );
        push_data( data, (LPVOID)h_extra->lparam, sizeof(KBDLLHOOKSTRUCT) );
        return 0;
    }
    case WM_WINE_MOUSE_LL_HOOK:
    {
        struct hook_extra_info *h_extra = (struct hook_extra_info *)lparam;
        data->ps.hook.handle = wine_server_user_handle( h_extra->handle );
        push_data( data, &data->ps.hook, sizeof(data->ps.hook) );
        push_data( data, (LPVOID)h_extra->lparam, sizeof(MSLLHOOKSTRUCT) );
        return 0;
    }
    case WM_NCPAINT:
        if (wparam <= 1) return 0;
        FIXME( "WM_NCPAINT hdc packing not supported yet\n" );
        data->count = -1;
        return 0;
    case WM_PAINT:
        if (!wparam) return 0;
        /* fall through */

    /* these contain an HFONT */
    case WM_SETFONT:
    case WM_GETFONT:
    /* these contain an HDC */
    case WM_ERASEBKGND:
    case WM_ICONERASEBKGND:
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
    case WM_PRINT:
    case WM_PRINTCLIENT:
    /* these contain an HGLOBAL */
    case WM_PAINTCLIPBOARD:
    case WM_SIZECLIPBOARD:
    /* these contain HICON */
    case WM_GETICON:
    case WM_SETICON:
    case WM_QUERYDRAGICON:
    case WM_QUERYPARKICON:
    /* these contain pointers */
    case WM_DROPOBJECT:
    case WM_QUERYDROPOBJECT:
    case WM_DRAGLOOP:
    case WM_DRAGSELECT:
    case WM_DRAGMOVE:
        FIXME( "msg %x (%s) not supported yet\n", message, SPY_GetMsgName(message, hwnd) );
        data->count = -1;
        return 0;
    }
    return 0;
}


/***********************************************************************
 *           handle_internal_message
 *
 * Handle an internal Wine message instead of calling the window proc.
 */
LRESULT handle_internal_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    MSG m;
    m.hwnd    = hwnd;
    m.message = msg;
    m.wParam  = wparam;
    m.lParam  = lparam;
    return NtUserCallOneParam( (UINT_PTR)&m, NtUserHandleInternalMessage );
}

/* since the WM_DDE_ACK response to a WM_DDE_EXECUTE message should contain the handle
 * to the memory handle, we keep track (in the server side) of all pairs of handle
 * used (the client passes its value and the content of the memory handle), and
 * the server stored both values (the client, and the local one, created after the
 * content). When a ACK message is generated, the list of pair is searched for a
 * matching pair, so that the client memory handle can be returned.
 */
struct DDE_pair {
    HGLOBAL     client_hMem;
    HGLOBAL     server_hMem;
};

static      struct DDE_pair*    dde_pairs;
static      int                 dde_num_alloc;
static      int                 dde_num_used;

static CRITICAL_SECTION dde_crst;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &dde_crst,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dde_crst") }
};
static CRITICAL_SECTION dde_crst = { &critsect_debug, -1, 0, 0, 0, 0 };

static BOOL dde_add_pair(HGLOBAL chm, HGLOBAL shm)
{
    int  i;
#define GROWBY  4

    EnterCriticalSection(&dde_crst);

    /* now remember the pair of hMem on both sides */
    if (dde_num_used == dde_num_alloc)
    {
        struct DDE_pair* tmp;
	if (dde_pairs)
	    tmp  = HeapReAlloc( GetProcessHeap(), 0, dde_pairs,
                                            (dde_num_alloc + GROWBY) * sizeof(struct DDE_pair));
	else
	    tmp  = HeapAlloc( GetProcessHeap(), 0, 
                                            (dde_num_alloc + GROWBY) * sizeof(struct DDE_pair));

        if (!tmp)
        {
            LeaveCriticalSection(&dde_crst);
            return FALSE;
        }
        dde_pairs = tmp;
        /* zero out newly allocated part */
        memset(&dde_pairs[dde_num_alloc], 0, GROWBY * sizeof(struct DDE_pair));
        dde_num_alloc += GROWBY;
    }
#undef GROWBY
    for (i = 0; i < dde_num_alloc; i++)
    {
        if (dde_pairs[i].server_hMem == 0)
        {
            dde_pairs[i].client_hMem = chm;
            dde_pairs[i].server_hMem = shm;
            dde_num_used++;
            break;
        }
    }
    LeaveCriticalSection(&dde_crst);
    return TRUE;
}

static HGLOBAL dde_get_pair(HGLOBAL shm)
{
    int  i;
    HGLOBAL     ret = 0;

    EnterCriticalSection(&dde_crst);
    for (i = 0; i < dde_num_alloc; i++)
    {
        if (dde_pairs[i].server_hMem == shm)
        {
            /* free this pair */
            dde_pairs[i].server_hMem = 0;
            dde_num_used--;
            ret = dde_pairs[i].client_hMem;
            break;
        }
    }
    LeaveCriticalSection(&dde_crst);
    return ret;
}

/***********************************************************************
 *		post_dde_message
 *
 * Post a DDE message
 */
BOOL post_dde_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, DWORD dest_tid, DWORD type )
{
    void*       ptr = NULL;
    int         size = 0;
    UINT_PTR    uiLo, uiHi;
    LPARAM      lp;
    HGLOBAL     hunlock = 0;
    DWORD       res;
    ULONGLONG   hpack;

    if (!UnpackDDElParam( msg, lparam, &uiLo, &uiHi ))
        return FALSE;

    lp = lparam;
    switch (msg)
    {
        /* DDE messages which don't require packing are:
         * WM_DDE_INITIATE
         * WM_DDE_TERMINATE
         * WM_DDE_REQUEST
         * WM_DDE_UNADVISE
         */
    case WM_DDE_ACK:
        if (HIWORD(uiHi))
        {
            /* uiHi should contain a hMem from WM_DDE_EXECUTE */
            HGLOBAL h = dde_get_pair( (HANDLE)uiHi );
            if (h)
            {
                hpack = pack_ptr( h );
                /* send back the value of h on the other side */
                ptr = &hpack;
                size = sizeof(hpack);
                lp = uiLo;
                TRACE( "send dde-ack %lx %08lx => %p\n", uiLo, uiHi, h );
            }
        }
        else
        {
            /* uiHi should contain either an atom or 0 */
            TRACE( "send dde-ack %lx atom=%lx\n", uiLo, uiHi );
            lp = MAKELONG( uiLo, uiHi );
        }
        break;
    case WM_DDE_ADVISE:
    case WM_DDE_DATA:
    case WM_DDE_POKE:
        if (uiLo)
        {
            size = GlobalSize( (HGLOBAL)uiLo ) ;
            if ((msg == WM_DDE_ADVISE && size < sizeof(DDEADVISE)) ||
                (msg == WM_DDE_DATA   && size < FIELD_OFFSET(DDEDATA, Value)) ||
                (msg == WM_DDE_POKE   && size < FIELD_OFFSET(DDEPOKE, Value)))
                return FALSE;
        }
        else if (msg != WM_DDE_DATA) return FALSE;

        lp = uiHi;
        if (uiLo)
        {
            if ((ptr = GlobalLock( (HGLOBAL)uiLo) ))
            {
                DDEDATA *dde_data = ptr;
                TRACE("unused %d, fResponse %d, fRelease %d, fDeferUpd %d, fAckReq %d, cfFormat %d\n",
                       dde_data->unused, dde_data->fResponse, dde_data->fRelease,
                       dde_data->reserved, dde_data->fAckReq, dde_data->cfFormat);
                hunlock = (HGLOBAL)uiLo;
            }
        }
        TRACE( "send ddepack %u %lx\n", size, uiHi );
        break;
    case WM_DDE_EXECUTE:
        if (lparam)
        {
            if ((ptr = GlobalLock( (HGLOBAL)lparam) ))
            {
                size = GlobalSize( (HGLOBAL)lparam );
                /* so that the other side can send it back on ACK */
                lp = lparam;
                hunlock = (HGLOBAL)lparam;
            }
        }
        break;
    }
    SERVER_START_REQ( send_message )
    {
        req->id      = dest_tid;
        req->type    = type;
        req->flags   = 0;
        req->win     = wine_server_user_handle( hwnd );
        req->msg     = msg;
        req->wparam  = wparam;
        req->lparam  = lp;
        req->timeout = TIMEOUT_INFINITE;
        if (size) wine_server_add_data( req, ptr, size );
        if ((res = wine_server_call( req )))
        {
            if (res == STATUS_INVALID_PARAMETER)
                /* FIXME: find a STATUS_ value for this one */
                SetLastError( ERROR_INVALID_THREAD_ID );
            else
                SetLastError( RtlNtStatusToDosError(res) );
        }
        else
            FreeDDElParam( msg, lparam );
    }
    SERVER_END_REQ;
    if (hunlock) GlobalUnlock(hunlock);

    return !res;
}

/***********************************************************************
 *		unpack_dde_message
 *
 * Unpack a posted DDE message received from another process.
 */
BOOL unpack_dde_message( HWND hwnd, UINT message, WPARAM *wparam, LPARAM *lparam,
                         void **buffer, size_t size )
{
    UINT_PTR	uiLo, uiHi;
    HGLOBAL	hMem = 0;
    void*	ptr;

    switch (message)
    {
    case WM_DDE_ACK:
        if (size)
        {
            ULONGLONG hpack;
            /* hMem is being passed */
            if (size != sizeof(hpack)) return FALSE;
            if (!buffer || !*buffer) return FALSE;
            uiLo = *lparam;
            memcpy( &hpack, *buffer, size );
            hMem = unpack_ptr( hpack );
            uiHi = (UINT_PTR)hMem;
            TRACE("recv dde-ack %lx mem=%lx[%lx]\n", uiLo, uiHi, GlobalSize( hMem ));
        }
        else
        {
            uiLo = LOWORD( *lparam );
            uiHi = HIWORD( *lparam );
            TRACE("recv dde-ack %lx atom=%lx\n", uiLo, uiHi);
        }
	*lparam = PackDDElParam( WM_DDE_ACK, uiLo, uiHi );
	break;
    case WM_DDE_ADVISE:
    case WM_DDE_DATA:
    case WM_DDE_POKE:
	if ((!buffer || !*buffer) && message != WM_DDE_DATA) return FALSE;
	uiHi = *lparam;
        if (size)
        {
            if (!(hMem = GlobalAlloc( GMEM_MOVEABLE|GMEM_DDESHARE, size )))
                return FALSE;
            if ((ptr = GlobalLock( hMem )))
            {
                memcpy( ptr, *buffer, size );
                GlobalUnlock( hMem );
            }
            else
            {
                GlobalFree( hMem );
                return FALSE;
            }
        }
        uiLo = (UINT_PTR)hMem;

	*lparam = PackDDElParam( message, uiLo, uiHi );
	break;
    case WM_DDE_EXECUTE:
	if (size)
	{
	    if (!buffer || !*buffer) return FALSE;
            if (!(hMem = GlobalAlloc( GMEM_MOVEABLE|GMEM_DDESHARE, size ))) return FALSE;
            if ((ptr = GlobalLock( hMem )))
	    {
		memcpy( ptr, *buffer, size );
		GlobalUnlock( hMem );
                TRACE( "exec: pairing c=%08lx s=%p\n", *lparam, hMem );
                if (!dde_add_pair( (HGLOBAL)*lparam, hMem ))
                {
                    GlobalFree( hMem );
                    return FALSE;
                }
            }
            else
            {
                GlobalFree( hMem );
                return FALSE;
            }
	} else return FALSE;
        *lparam = (LPARAM)hMem;
        break;
    }
    return TRUE;
}

/***********************************************************************
 *           send_parent_notify
 *
 * Send a WM_PARENTNOTIFY to all ancestors of the given window, unless
 * the window has the WS_EX_NOPARENTNOTIFY style.
 */
static void send_parent_notify( HWND hwnd, WORD event, WORD idChild, POINT pt )
{
    /* pt has to be in the client coordinates of the parent window */
    MapWindowPoints( 0, hwnd, &pt, 1 );
    for (;;)
    {
        HWND parent;

        if (!(GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD)) break;
        if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_NOPARENTNOTIFY) break;
        if (!(parent = GetParent(hwnd))) break;
        if (parent == GetDesktopWindow()) break;
        MapWindowPoints( hwnd, parent, &pt, 1 );
        hwnd = parent;
        SendMessageW( hwnd, WM_PARENTNOTIFY,
                      MAKEWPARAM( event, idChild ), MAKELPARAM( pt.x, pt.y ) );
    }
}


/***********************************************************************
 *          accept_hardware_message
 *
 * Tell the server we have passed the message to the app
 * (even though we may end up dropping it later on)
 */
static void accept_hardware_message( UINT hw_id )
{
    SERVER_START_REQ( accept_hardware_message )
    {
        req->hw_id = hw_id;
        if (wine_server_call( req ))
            FIXME("Failed to reply to MSG_HARDWARE message. Message may not be removed from queue.\n");
    }
    SERVER_END_REQ;
}


static BOOL process_rawinput_message( MSG *msg, UINT hw_id, const struct hardware_msg_data *msg_data )
{
    struct rawinput_thread_data *thread_data = rawinput_thread_data();

    if (msg->message == WM_INPUT_DEVICE_CHANGE)
        rawinput_update_device_list();
    else
    {
        thread_data->buffer->header.dwSize = RAWINPUT_BUFFER_SIZE;
        if (!rawinput_from_hardware_message( thread_data->buffer, msg_data )) return FALSE;
        thread_data->hw_id = hw_id;
        msg->lParam = (LPARAM)hw_id;
    }

    msg->pt = point_phys_to_win_dpi( msg->hwnd, msg->pt );
    return TRUE;
}

/***********************************************************************
 *          process_keyboard_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
static BOOL process_keyboard_message( MSG *msg, UINT hw_id, HWND hwnd_filter,
                                      UINT first, UINT last, BOOL remove )
{
    EVENTMSG event;

    if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN ||
        msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP)
        switch (msg->wParam)
        {
            case VK_LSHIFT: case VK_RSHIFT:
                msg->wParam = VK_SHIFT;
                break;
            case VK_LCONTROL: case VK_RCONTROL:
                msg->wParam = VK_CONTROL;
                break;
            case VK_LMENU: case VK_RMENU:
                msg->wParam = VK_MENU;
                break;
        }

    /* FIXME: is this really the right place for this hook? */
    event.message = msg->message;
    event.hwnd    = msg->hwnd;
    event.time    = msg->time;
    event.paramL  = (msg->wParam & 0xFF) | (HIWORD(msg->lParam) << 8);
    event.paramH  = msg->lParam & 0x7FFF;
    if (HIWORD(msg->lParam) & 0x0100) event.paramH |= 0x8000; /* special_key - bit */
    HOOK_CallHooks( WH_JOURNALRECORD, HC_ACTION, 0, (LPARAM)&event, TRUE );

    /* check message filters */
    if (msg->message < first || msg->message > last) return FALSE;
    if (!check_hwnd_filter( msg, hwnd_filter )) return FALSE;

    if (remove)
    {
        if((msg->message == WM_KEYDOWN) &&
           (msg->hwnd != GetDesktopWindow()))
        {
            /* Handle F1 key by sending out WM_HELP message */
            if (msg->wParam == VK_F1)
            {
                PostMessageW( msg->hwnd, WM_KEYF1, 0, 0 );
            }
            else if(msg->wParam >= VK_BROWSER_BACK &&
                    msg->wParam <= VK_LAUNCH_APP2)
            {
                /* FIXME: Process keystate */
                SendMessageW(msg->hwnd, WM_APPCOMMAND, (WPARAM)msg->hwnd, MAKELPARAM(0, (FAPPCOMMAND_KEY | (msg->wParam - VK_BROWSER_BACK + 1))));
            }
        }
        else if (msg->message == WM_KEYUP)
        {
            /* Handle VK_APPS key by posting a WM_CONTEXTMENU message */
            if (msg->wParam == VK_APPS && !MENU_IsMenuActive())
                PostMessageW(msg->hwnd, WM_CONTEXTMENU, (WPARAM)msg->hwnd, -1);
        }
    }

    if (HOOK_CallHooks( WH_KEYBOARD, remove ? HC_ACTION : HC_NOREMOVE,
                        LOWORD(msg->wParam), msg->lParam, TRUE ))
    {
        /* skip this message */
        HOOK_CallHooks( WH_CBT, HCBT_KEYSKIPPED, LOWORD(msg->wParam), msg->lParam, TRUE );
        accept_hardware_message( hw_id );
        return FALSE;
    }
    if (remove) accept_hardware_message( hw_id );
    msg->pt = point_phys_to_win_dpi( msg->hwnd, msg->pt );

    if ( remove && msg->message == WM_KEYDOWN )
        if (ImmProcessKey(msg->hwnd, NtUserGetKeyboardLayout(0), msg->wParam, msg->lParam, 0) )
            msg->wParam = VK_PROCESSKEY;

    return TRUE;
}


/***********************************************************************
 *          process_mouse_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
static BOOL process_mouse_message( MSG *msg, UINT hw_id, ULONG_PTR extra_info, HWND hwnd_filter,
                                   UINT first, UINT last, BOOL remove )
{
    static MSG clk_msg;

    POINT pt;
    UINT message;
    INT hittest;
    EVENTMSG event;
    GUITHREADINFO info;
    MOUSEHOOKSTRUCTEX hook;
    BOOL eatMsg;
    WPARAM wparam;

    /* find the window to dispatch this mouse message to */

    info.cbSize = sizeof(info);
    NtUserGetGUIThreadInfo( GetCurrentThreadId(), &info );
    if (info.hwndCapture)
    {
        hittest = HTCLIENT;
        msg->hwnd = info.hwndCapture;
    }
    else
    {
        HWND orig = msg->hwnd;

        msg->hwnd = WINPOS_WindowFromPoint( msg->hwnd, msg->pt, &hittest );
        if (!msg->hwnd) /* As a heuristic, try the next window if it's the owner of orig */
        {
            HWND next = GetWindow( orig, GW_HWNDNEXT );

            if (next && GetWindow( orig, GW_OWNER ) == next && WIN_IsCurrentThread( next ))
                msg->hwnd = WINPOS_WindowFromPoint( next, msg->pt, &hittest );
        }
    }

    if (!msg->hwnd || !WIN_IsCurrentThread( msg->hwnd ))
    {
        accept_hardware_message( hw_id );
        return FALSE;
    }

    msg->pt = point_phys_to_win_dpi( msg->hwnd, msg->pt );
    SetThreadDpiAwarenessContext( GetWindowDpiAwarenessContext( msg->hwnd ));

    /* FIXME: is this really the right place for this hook? */
    event.message = msg->message;
    event.time    = msg->time;
    event.hwnd    = msg->hwnd;
    event.paramL  = msg->pt.x;
    event.paramH  = msg->pt.y;
    HOOK_CallHooks( WH_JOURNALRECORD, HC_ACTION, 0, (LPARAM)&event, TRUE );

    if (!check_hwnd_filter( msg, hwnd_filter )) return FALSE;

    pt = msg->pt;
    message = msg->message;
    wparam = msg->wParam;
    /* Note: windows has no concept of a non-client wheel message */
    if (message != WM_MOUSEWHEEL)
    {
        if (hittest != HTCLIENT)
        {
            message += WM_NCMOUSEMOVE - WM_MOUSEMOVE;
            wparam = hittest;
        }
        else
        {
            /* coordinates don't get translated while tracking a menu */
            /* FIXME: should differentiate popups and top-level menus */
            if (!(info.flags & GUI_INMENUMODE))
                ScreenToClient( msg->hwnd, &pt );
        }
    }
    msg->lParam = MAKELONG( pt.x, pt.y );

    /* translate double clicks */

    if ((msg->message == WM_LBUTTONDOWN) ||
        (msg->message == WM_RBUTTONDOWN) ||
        (msg->message == WM_MBUTTONDOWN) ||
        (msg->message == WM_XBUTTONDOWN))
    {
        BOOL update = remove;

        /* translate double clicks -
	 * note that ...MOUSEMOVEs can slip in between
	 * ...BUTTONDOWN and ...BUTTONDBLCLK messages */

        if ((info.flags & (GUI_INMENUMODE|GUI_INMOVESIZE)) ||
            hittest != HTCLIENT ||
            (GetClassLongA( msg->hwnd, GCL_STYLE ) & CS_DBLCLKS))
        {
           if ((msg->message == clk_msg.message) &&
               (msg->hwnd == clk_msg.hwnd) &&
               (msg->wParam == clk_msg.wParam) &&
               (msg->time - clk_msg.time < NtUserGetDoubleClickTime()) &&
               (abs(msg->pt.x - clk_msg.pt.x) < GetSystemMetrics(SM_CXDOUBLECLK)/2) &&
               (abs(msg->pt.y - clk_msg.pt.y) < GetSystemMetrics(SM_CYDOUBLECLK)/2))
           {
               message += (WM_LBUTTONDBLCLK - WM_LBUTTONDOWN);
               if (update)
               {
                   clk_msg.message = 0;  /* clear the double click conditions */
                   update = FALSE;
               }
           }
        }
        if (message < first || message > last) return FALSE;
        /* update static double click conditions */
        if (update) clk_msg = *msg;
    }
    else
    {
        if (message < first || message > last) return FALSE;
    }
    msg->wParam = wparam;

    /* message is accepted now (but may still get dropped) */

    hook.s.pt           = msg->pt;
    hook.s.hwnd         = msg->hwnd;
    hook.s.wHitTestCode = hittest;
    hook.s.dwExtraInfo  = extra_info;
    hook.mouseData      = msg->wParam;
    if (HOOK_CallHooks( WH_MOUSE, remove ? HC_ACTION : HC_NOREMOVE,
                        message, (LPARAM)&hook, TRUE ))
    {
        hook.s.pt           = msg->pt;
        hook.s.hwnd         = msg->hwnd;
        hook.s.wHitTestCode = hittest;
        hook.s.dwExtraInfo  = extra_info;
        hook.mouseData      = msg->wParam;
        HOOK_CallHooks( WH_CBT, HCBT_CLICKSKIPPED, message, (LPARAM)&hook, TRUE );
        accept_hardware_message( hw_id );
        return FALSE;
    }

    if ((hittest == HTERROR) || (hittest == HTNOWHERE))
    {
        SendMessageW( msg->hwnd, WM_SETCURSOR, (WPARAM)msg->hwnd,
                      MAKELONG( hittest, msg->message ));
        accept_hardware_message( hw_id );
        return FALSE;
    }

    if (remove) accept_hardware_message( hw_id );

    if (!remove || info.hwndCapture)
    {
        msg->message = message;
        return TRUE;
    }

    eatMsg = FALSE;

    if ((msg->message == WM_LBUTTONDOWN) ||
        (msg->message == WM_RBUTTONDOWN) ||
        (msg->message == WM_MBUTTONDOWN) ||
        (msg->message == WM_XBUTTONDOWN))
    {
        /* Send the WM_PARENTNOTIFY,
         * note that even for double/nonclient clicks
         * notification message is still WM_L/M/RBUTTONDOWN.
         */
        send_parent_notify( msg->hwnd, msg->message, 0, msg->pt );

        /* Activate the window if needed */

        if (msg->hwnd != info.hwndActive)
        {
            HWND hwndTop = NtUserGetAncestor( msg->hwnd, GA_ROOT );

            if ((GetWindowLongW( hwndTop, GWL_STYLE ) & (WS_POPUP|WS_CHILD)) != WS_CHILD)
            {
                LONG ret = SendMessageW( msg->hwnd, WM_MOUSEACTIVATE, (WPARAM)hwndTop,
                                         MAKELONG( hittest, msg->message ) );
                switch(ret)
                {
                case MA_NOACTIVATEANDEAT:
                    eatMsg = TRUE;
                    /* fall through */
                case MA_NOACTIVATE:
                    break;
                case MA_ACTIVATEANDEAT:
                    eatMsg = TRUE;
                    /* fall through */
                case MA_ACTIVATE:
                case 0:
                    if (!FOCUS_MouseActivate( hwndTop )) eatMsg = TRUE;
                    break;
                default:
                    WARN( "unknown WM_MOUSEACTIVATE code %d\n", ret );
                    break;
                }
            }
        }
    }

    /* send the WM_SETCURSOR message */

    /* Windows sends the normal mouse message as the message parameter
       in the WM_SETCURSOR message even if it's non-client mouse message */
    SendMessageW( msg->hwnd, WM_SETCURSOR, (WPARAM)msg->hwnd, MAKELONG( hittest, msg->message ));

    msg->message = message;
    return !eatMsg;
}


/***********************************************************************
 *           process_hardware_message
 *
 * Process a hardware message; return TRUE if message should be passed on to the app
 */
BOOL process_hardware_message( MSG *msg, UINT hw_id, const struct hardware_msg_data *msg_data,
                               HWND hwnd_filter, UINT first, UINT last, BOOL remove )
{
    DPI_AWARENESS_CONTEXT context;
    BOOL ret = FALSE;

    get_user_thread_info()->msg_source.deviceType = msg_data->source.device;
    get_user_thread_info()->msg_source.originId   = msg_data->source.origin;

    /* hardware messages are always in physical coords */
    context = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE );

    if (msg->message == WM_INPUT || msg->message == WM_INPUT_DEVICE_CHANGE)
        ret = process_rawinput_message( msg, hw_id, msg_data );
    else if (is_keyboard_message( msg->message ))
        ret = process_keyboard_message( msg, hw_id, hwnd_filter, first, last, remove );
    else if (is_mouse_message( msg->message ))
        ret = process_mouse_message( msg, hw_id, msg_data->info, hwnd_filter, first, last, remove );
    else
        ERR( "unknown message type %x\n", msg->message );
    SetThreadDpiAwarenessContext( context );
    return ret;
}


/***********************************************************************
 *		put_message_in_queue
 *
 * Put a sent message into the destination queue.
 * For inter-process message, reply_size is set to expected size of reply data.
 */
static BOOL put_message_in_queue( const struct send_message_info *info, size_t *reply_size )
{
    struct packed_message data;
    message_data_t msg_data;
    unsigned int res;
    int i;
    timeout_t timeout = TIMEOUT_INFINITE;

    /* Check for INFINITE timeout for compatibility with Win9x,
     * although Windows >= NT does not do so
     */
    if (info->type != MSG_NOTIFY &&
        info->type != MSG_CALLBACK &&
        info->type != MSG_POSTED &&
        info->timeout &&
        info->timeout != INFINITE)
    {
        /* timeout is signed despite the prototype */
        timeout = (timeout_t)max( 0, (int)info->timeout ) * -10000;
    }

    memset( &data, 0, sizeof(data) );
    if (info->type == MSG_OTHER_PROCESS || info->type == MSG_NOTIFY)
    {
        *reply_size = pack_message( info->hwnd, info->msg, info->wparam, info->lparam, &data );
        if (data.count == -1)
        {
            WARN( "cannot pack message %x\n", info->msg );
            return FALSE;
        }
    }
    else if (info->type == MSG_CALLBACK)
    {
        msg_data.callback.callback = wine_server_client_ptr( info->callback );
        msg_data.callback.data     = info->data;
        msg_data.callback.result   = 0;
        data.data[0] = &msg_data;
        data.size[0] = sizeof(msg_data.callback);
        data.count = 1;
    }
    else if (info->type == MSG_POSTED && info->msg >= WM_DDE_FIRST && info->msg <= WM_DDE_LAST)
    {
        return post_dde_message( info->hwnd, info->msg, info->wparam, info->lparam,
                                 info->dest_tid, info->type );
    }

    SERVER_START_REQ( send_message )
    {
        req->id      = info->dest_tid;
        req->type    = info->type;
        req->flags   = 0;
        req->win     = wine_server_user_handle( info->hwnd );
        req->msg     = info->msg;
        req->wparam  = info->wparam;
        req->lparam  = info->lparam;
        req->timeout = timeout;

        if (info->flags & SMTO_ABORTIFHUNG) req->flags |= SEND_MSG_ABORT_IF_HUNG;
        for (i = 0; i < data.count; i++) wine_server_add_data( req, data.data[i], data.size[i] );
        if ((res = wine_server_call( req )))
        {
            if (res == STATUS_INVALID_PARAMETER)
                /* FIXME: find a STATUS_ value for this one */
                SetLastError( ERROR_INVALID_THREAD_ID );
            else
                SetLastError( RtlNtStatusToDosError(res) );
        }
    }
    SERVER_END_REQ;
    return !res;
}


static BOOL is_message_broadcastable(UINT msg)
{
    return msg < WM_USER || msg >= 0xc000;
}

/***********************************************************************
 *		SendMessageTimeoutW  (USER32.@)
 */
LRESULT WINAPI SendMessageTimeoutW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                    UINT flags, UINT timeout, PDWORD_PTR res_ptr )
{
    struct send_message_timeout_params params = { .flags = flags, .timeout = timeout };
    LRESULT res;

    res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params, FNID_SENDMESSAGEWTOOPTION, FALSE );
    if (res_ptr) *res_ptr = res;
    return params.result;
}

/***********************************************************************
 *		SendMessageTimeoutA  (USER32.@)
 */
LRESULT WINAPI SendMessageTimeoutA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                    UINT flags, UINT timeout, PDWORD_PTR res_ptr )
{
    struct send_message_timeout_params params = { .flags = flags, .timeout = timeout };
    LRESULT res = 0;

    if (msg != WM_CHAR || WIN_IsCurrentThread( hwnd ))
    {
        res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params,
                                 FNID_SENDMESSAGEWTOOPTION, TRUE );
    }
    else if (map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
    {
        res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params,
                                 FNID_SENDMESSAGEWTOOPTION, FALSE );
    }

    if (res_ptr) *res_ptr = res;
    return params.result;
}


/***********************************************************************
 *		SendMessageW  (USER32.@)
 */
LRESULT WINAPI SendMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    return NtUserMessageCall( hwnd, msg, wparam, lparam, NULL, FNID_SENDMESSAGE, FALSE );
}


/***********************************************************************
 *		SendMessageA  (USER32.@)
 */
LRESULT WINAPI SendMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_CHAR && !WIN_IsCurrentThread( hwnd ))
    {
        if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
            return 0;
        return NtUserMessageCall( hwnd, msg, wparam, lparam, NULL, FNID_SENDMESSAGE, FALSE );
    }

    return NtUserMessageCall( hwnd, msg, wparam, lparam, NULL, FNID_SENDMESSAGE, TRUE );
}


/***********************************************************************
 *		SendNotifyMessageA  (USER32.@)
 */
BOOL WINAPI SendNotifyMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!WIN_IsCurrentThread( hwnd ) && !map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
        return FALSE;

    return NtUserMessageCall( hwnd, msg, wparam, lparam, 0, FNID_SENDNOTIFYMESSAGE, TRUE );
}


/***********************************************************************
 *		SendNotifyMessageW  (USER32.@)
 */
BOOL WINAPI SendNotifyMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    return NtUserMessageCall( hwnd, msg, wparam, lparam, 0, FNID_SENDNOTIFYMESSAGE, FALSE );
}


/***********************************************************************
 *		SendMessageCallbackA  (USER32.@)
 */
BOOL WINAPI SendMessageCallbackA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                  SENDASYNCPROC callback, ULONG_PTR data )
{
    struct send_message_callback_params params = { .callback = callback, .data = data };

    if (!WIN_IsCurrentThread( hwnd ) && !map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
        return FALSE;

    return NtUserMessageCall( hwnd, msg, wparam, lparam, &params, FNID_SENDMESSAGECALLBACK, TRUE );
}


/***********************************************************************
 *		SendMessageCallbackW  (USER32.@)
 */
BOOL WINAPI SendMessageCallbackW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                  SENDASYNCPROC callback, ULONG_PTR data )
{
    struct send_message_callback_params params = { .callback = callback, .data = data };
    return NtUserMessageCall( hwnd, msg, wparam, lparam, &params, FNID_SENDMESSAGECALLBACK, FALSE );
}


/***********************************************************************
 *		ReplyMessage  (USER32.@)
 */
BOOL WINAPI ReplyMessage( LRESULT result )
{
    return NtUserCallTwoParam( result, 0, NtUserReplyMessage );
}


/***********************************************************************
 *		InSendMessage  (USER32.@)
 */
BOOL WINAPI InSendMessage(void)
{
    return (InSendMessageEx( NULL ) & (ISMEX_SEND | ISMEX_NOTIFY | ISMEX_CALLBACK)) != 0;
}


/***********************************************************************
 *		InSendMessageEx  (USER32.@)
 */
DWORD WINAPI InSendMessageEx( LPVOID reserved )
{
    struct received_message_info *info = get_user_thread_info()->receive_info;

    if (info) return info->flags;
    return ISMEX_NOSEND;
}


/***********************************************************************
 *		PostMessageA  (USER32.@)
 */
BOOL WINAPI PostMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_POSTMESSAGE )) return TRUE;
    return PostMessageW( hwnd, msg, wparam, lparam );
}


/***********************************************************************
 *		PostMessageW  (USER32.@)
 */
BOOL WINAPI PostMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct send_message_info info;

    if (is_pointer_message( msg, wparam ))
    {
        SetLastError( ERROR_MESSAGE_SYNC_ONLY );
        return FALSE;
    }

    TRACE( "hwnd %p msg %x (%s) wp %lx lp %lx\n",
           hwnd, msg, SPY_GetMsgName(msg, hwnd), wparam, lparam );

    info.type   = MSG_POSTED;
    info.hwnd   = hwnd;
    info.msg    = msg;
    info.wparam = wparam;
    info.lparam = lparam;
    info.flags  = 0;

    if (is_broadcast(hwnd))
    {
        if (is_message_broadcastable( info.msg ))
            EnumWindows( broadcast_message_callback, (LPARAM)&info );
        return TRUE;
    }

    if (!hwnd) return PostThreadMessageW( GetCurrentThreadId(), msg, wparam, lparam );

    if (!(info.dest_tid = GetWindowThreadProcessId( hwnd, NULL ))) return FALSE;

    if (USER_IsExitingThread( info.dest_tid )) return TRUE;

    return put_message_in_queue( &info, NULL );
}


/**********************************************************************
 *		PostThreadMessageA  (USER32.@)
 */
BOOL WINAPI PostThreadMessageA( DWORD thread, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_POSTMESSAGE )) return TRUE;
    return PostThreadMessageW( thread, msg, wparam, lparam );
}


/**********************************************************************
 *		PostThreadMessageW  (USER32.@)
 */
BOOL WINAPI PostThreadMessageW( DWORD thread, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct send_message_info info;

    if (is_pointer_message( msg, wparam ))
    {
        SetLastError( ERROR_MESSAGE_SYNC_ONLY );
        return FALSE;
    }
    if (USER_IsExitingThread( thread )) return TRUE;

    info.type     = MSG_POSTED;
    info.dest_tid = thread;
    info.hwnd     = 0;
    info.msg      = msg;
    info.wparam   = wparam;
    info.lparam   = lparam;
    info.flags    = 0;
    return put_message_in_queue( &info, NULL );
}


/***********************************************************************
 *		PostQuitMessage  (USER32.@)
 *
 * Posts a quit message to the current thread's message queue.
 *
 * PARAMS
 *  exit_code [I] Exit code to return from message loop.
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 *  This function is not the same as calling:
 *|PostThreadMessage(GetCurrentThreadId(), WM_QUIT, exit_code, 0);
 *  It instead sets a flag in the message queue that signals it to generate
 *  a WM_QUIT message when there are no other pending sent or posted messages
 *  in the queue.
 */
void WINAPI PostQuitMessage( INT exit_code )
{
    SERVER_START_REQ( post_quit_message )
    {
        req->exit_code = exit_code;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

/***********************************************************************
 *		PeekMessageW  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekMessageW( MSG *msg_out, HWND hwnd, UINT first, UINT last, UINT flags )
{
    return NtUserPeekMessage( msg_out, hwnd, first, last, flags );
}


/***********************************************************************
 *		PeekMessageA  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekMessageA( MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags )
{
    if (get_pending_wmchar( msg, first, last, (flags & PM_REMOVE) )) return TRUE;
    if (!PeekMessageW( msg, hwnd, first, last, flags )) return FALSE;
    map_wparam_WtoA( msg, (flags & PM_REMOVE) );
    return TRUE;
}


/***********************************************************************
 *		GetMessageW  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetMessageW( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    return NtUserGetMessage( msg, hwnd, first, last );
}


/***********************************************************************
 *		GetMessageA  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetMessageA( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    if (get_pending_wmchar( msg, first, last, TRUE )) return TRUE;
    if (GetMessageW( msg, hwnd, first, last ) < 0) return -1;
    map_wparam_WtoA( msg, TRUE );
    return (msg->message != WM_QUIT);
}


/***********************************************************************
 *		IsDialogMessageA (USER32.@)
 *		IsDialogMessage  (USER32.@)
 */
BOOL WINAPI IsDialogMessageA( HWND hwndDlg, LPMSG pmsg )
{
    MSG msg = *pmsg;
    map_wparam_AtoW( msg.message, &msg.wParam, WMCHAR_MAP_NOMAPPING );
    return IsDialogMessageW( hwndDlg, &msg );
}


/***********************************************************************
 *		TranslateMessage (USER32.@)
 *
 * Implementation of TranslateMessage.
 *
 * TranslateMessage translates virtual-key messages into character-messages,
 * as follows :
 * WM_KEYDOWN/WM_KEYUP combinations produce a WM_CHAR or WM_DEADCHAR message.
 * ditto replacing WM_* with WM_SYS*
 * This produces WM_CHAR messages only for keys mapped to ASCII characters
 * by the keyboard driver.
 *
 * If the message is WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, or WM_SYSKEYUP, the
 * return value is nonzero, regardless of the translation.
 *
 */
BOOL WINAPI TranslateMessage( const MSG *msg )
{
    UINT message;
    WCHAR wp[8];
    BYTE state[256];
    INT len;

    if (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST) return FALSE;
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return TRUE;

    TRACE_(key)("Translating key %s (%04lX), scancode %04x\n",
                SPY_GetVKeyName(msg->wParam), msg->wParam, HIWORD(msg->lParam));

    switch (msg->wParam)
    {
    case VK_PACKET:
        message = (msg->message == WM_KEYDOWN) ? WM_CHAR : WM_SYSCHAR;
        TRACE_(key)("PostMessageW(%p,%s,%04x,%08x)\n",
                    msg->hwnd, SPY_GetMsgName(message, msg->hwnd), HIWORD(msg->lParam), LOWORD(msg->lParam));
        PostMessageW( msg->hwnd, message, HIWORD(msg->lParam), LOWORD(msg->lParam));
        return TRUE;

    case VK_PROCESSKEY:
        return ImmTranslateMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }

    NtUserGetKeyboardState( state );
    len = ToUnicode(msg->wParam, HIWORD(msg->lParam), state, wp, ARRAY_SIZE(wp), 0);
    if (len == -1)
    {
        message = (msg->message == WM_KEYDOWN) ? WM_DEADCHAR : WM_SYSDEADCHAR;
        TRACE_(key)("-1 -> PostMessageW(%p,%s,%04x,%08lx)\n",
            msg->hwnd, SPY_GetMsgName(message, msg->hwnd), wp[0], msg->lParam);
        PostMessageW( msg->hwnd, message, wp[0], msg->lParam );
    }
    else if (len > 0)
    {
        INT i;

        message = (msg->message == WM_KEYDOWN) ? WM_CHAR : WM_SYSCHAR;
        TRACE_(key)("%d -> PostMessageW(%p,%s,<x>,%08lx) for <x> in %s\n", len, msg->hwnd,
            SPY_GetMsgName(message, msg->hwnd), msg->lParam, debugstr_wn(wp, len));
        for (i = 0; i < len; i++)
            PostMessageW( msg->hwnd, message, wp[i], msg->lParam );
    }
    return TRUE;
}


/***********************************************************************
 *		DispatchMessageA (USER32.@)
 *
 * See DispatchMessageW.
 */
LRESULT WINAPI DECLSPEC_HOTPATCH DispatchMessageA( const MSG* msg )
{
    LRESULT retval;

      /* Process timer messages */
    if ((msg->message == WM_TIMER) || (msg->message == WM_SYSTIMER))
    {
        if (msg->lParam)
        {
            __TRY
            {
                retval = CallWindowProcA( (WNDPROC)msg->lParam, msg->hwnd,
                                          msg->message, msg->wParam, GetTickCount() );
            }
            __EXCEPT_ALL
            {
                retval = 0;
            }
            __ENDTRY
            return retval;
        }
    }
    return NtUserCallOneParam( (UINT_PTR)msg, NtUserDispatchMessageA );
}


/***********************************************************************
 *		DispatchMessageW (USER32.@) Process a message
 *
 * Process the message specified in the structure *_msg_.
 *
 * If the lpMsg parameter points to a WM_TIMER message and the
 * parameter of the WM_TIMER message is not NULL, the lParam parameter
 * points to the function that is called instead of the window
 * procedure. The function stored in lParam (timer callback) is protected
 * from causing page-faults.
 *
 * The message must be valid.
 *
 * RETURNS
 *
 *   DispatchMessage() returns the result of the window procedure invoked.
 *
 * CONFORMANCE
 *
 *   ECMA-234, Win32
 *
 */
LRESULT WINAPI DECLSPEC_HOTPATCH DispatchMessageW( const MSG* msg )
{
    LRESULT retval;

      /* Process timer messages */
    if ((msg->message == WM_TIMER) || (msg->message == WM_SYSTIMER))
    {
        if (msg->lParam)
        {
            __TRY
            {
                retval = CallWindowProcW( (WNDPROC)msg->lParam, msg->hwnd,
                                          msg->message, msg->wParam, GetTickCount() );
            }
            __EXCEPT_ALL
            {
                retval = 0;
            }
            __ENDTRY
            return retval;
        }
    }
    return NtUserDispatchMessage( msg );
}


/***********************************************************************
 *		GetMessagePos (USER.119)
 *		GetMessagePos (USER32.@)
 *
 * The GetMessagePos() function returns a long value representing a
 * cursor position, in screen coordinates, when the last message
 * retrieved by the GetMessage() function occurs. The x-coordinate is
 * in the low-order word of the return value, the y-coordinate is in
 * the high-order word. The application can use the MAKEPOINT()
 * macro to obtain a POINT structure from the return value.
 *
 * For the current cursor position, use GetCursorPos().
 *
 * RETURNS
 *
 * Cursor position of last message on success, zero on failure.
 *
 * CONFORMANCE
 *
 * ECMA-234, Win32
 *
 */
DWORD WINAPI GetMessagePos(void)
{
    return get_user_thread_info()->GetMessagePosVal;
}


/***********************************************************************
 *		GetMessageTime (USER.120)
 *		GetMessageTime (USER32.@)
 *
 * GetMessageTime() returns the message time for the last message
 * retrieved by the function. The time is measured in milliseconds with
 * the same offset as GetTickCount().
 *
 * Since the tick count wraps, this is only useful for moderately short
 * relative time comparisons.
 *
 * RETURNS
 *
 * Time of last message on success, zero on failure.
 */
LONG WINAPI GetMessageTime(void)
{
    return get_user_thread_info()->GetMessageTimeVal;
}


/***********************************************************************
 *		GetMessageExtraInfo (USER.288)
 *		GetMessageExtraInfo (USER32.@)
 */
LPARAM WINAPI GetMessageExtraInfo(void)
{
    return get_user_thread_info()->GetMessageExtraInfoVal;
}


/***********************************************************************
 *		SetMessageExtraInfo (USER32.@)
 */
LPARAM WINAPI SetMessageExtraInfo(LPARAM lParam)
{
    struct user_thread_info *thread_info = get_user_thread_info();
    LONG old_value = thread_info->GetMessageExtraInfoVal;
    thread_info->GetMessageExtraInfoVal = lParam;
    return old_value;
}


/***********************************************************************
 *		GetCurrentInputMessageSource (USER32.@)
 */
BOOL WINAPI GetCurrentInputMessageSource( INPUT_MESSAGE_SOURCE *source )
{
    *source = get_user_thread_info()->msg_source;
    return TRUE;
}


/***********************************************************************
 *		WaitMessage (USER.112) Suspend thread pending messages
 *		WaitMessage (USER32.@) Suspend thread pending messages
 *
 * WaitMessage() suspends a thread until events appear in the thread's
 * queue.
 */
BOOL WINAPI WaitMessage(void)
{
    return NtUserMsgWaitForMultipleObjectsEx( 0, NULL, INFINITE, QS_ALLINPUT, 0 ) != WAIT_FAILED;
}


/***********************************************************************
 *		MsgWaitForMultipleObjects (USER32.@)
 */
DWORD WINAPI MsgWaitForMultipleObjects( DWORD count, const HANDLE *handles,
                                        BOOL wait_all, DWORD timeout, DWORD mask )
{
    return NtUserMsgWaitForMultipleObjectsEx( count, handles, timeout, mask,
                                              wait_all ? MWMO_WAITALL : 0 );
}


/***********************************************************************
 *		WaitForInputIdle (USER32.@)
 */
DWORD WINAPI WaitForInputIdle( HANDLE process, DWORD timeout )
{
    return NtUserWaitForInputIdle( process, timeout, FALSE );
}


/***********************************************************************
 *		RegisterWindowMessageA (USER32.@)
 *		RegisterWindowMessage (USER.118)
 */
UINT WINAPI RegisterWindowMessageA( LPCSTR str )
{
    UINT ret = GlobalAddAtomA(str);
    TRACE("%s, ret=%x\n", str, ret);
    return ret;
}


/***********************************************************************
 *		RegisterWindowMessageW (USER32.@)
 */
UINT WINAPI RegisterWindowMessageW( LPCWSTR str )
{
    UINT ret = GlobalAddAtomW(str);
    TRACE("%s ret=%x\n", debugstr_w(str), ret);
    return ret;
}

typedef struct BroadcastParm
{
    DWORD flags;
    LPDWORD recipients;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    BOOL success;
    HWINSTA winsta;
} BroadcastParm;

static BOOL CALLBACK bcast_childwindow( HWND hw, LPARAM lp )
{
    BroadcastParm *parm = (BroadcastParm*)lp;
    DWORD_PTR retval = 0;
    LRESULT lresult;

    if (parm->flags & BSF_IGNORECURRENTTASK && WIN_IsCurrentProcess(hw))
    {
        TRACE("Not telling myself %p\n", hw);
        return TRUE;
    }

    /* I don't know 100% for sure if this is what Windows does, but it fits the tests */
    if (parm->flags & BSF_QUERY)
    {
        TRACE("Telling window %p using SendMessageTimeout\n", hw);

        /* Not tested for conflicting flags */
        if (parm->flags & BSF_FORCEIFHUNG || parm->flags & BSF_NOHANG)
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_ABORTIFHUNG, 2000, &retval );
        else if (parm->flags & BSF_NOTIMEOUTIFNOTHUNG)
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_NOTIMEOUTIFNOTHUNG, 2000, &retval );
        else
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_NORMAL, 2000, &retval );

        if (!lresult && GetLastError() == ERROR_TIMEOUT)
        {
            WARN("Timed out!\n");
            if (!(parm->flags & BSF_FORCEIFHUNG))
                goto fail;
        }
        if (retval == BROADCAST_QUERY_DENY)
            goto fail;

        return TRUE;

fail:
        parm->success = FALSE;
        return FALSE;
    }
    else if (parm->flags & BSF_POSTMESSAGE)
    {
        TRACE("Telling window %p using PostMessage\n", hw);
        PostMessageW( hw, parm->msg, parm->wp, parm->lp );
    }
    else
    {
        TRACE("Telling window %p using SendNotifyMessage\n", hw);
        SendNotifyMessageW( hw, parm->msg, parm->wp, parm->lp );
    }

    return TRUE;
}

static BOOL CALLBACK bcast_desktop( LPWSTR desktop, LPARAM lp )
{
    BOOL ret;
    HDESK hdesktop;
    BroadcastParm *parm = (BroadcastParm*)lp;

    TRACE("desktop: %s\n", debugstr_w( desktop ));

    hdesktop = open_winstation_desktop( parm->winsta, desktop, 0, FALSE, DESKTOP_ENUMERATE|DESKTOP_WRITEOBJECTS|STANDARD_RIGHTS_WRITE );
    if (!hdesktop)
    {
        FIXME("Could not open desktop %s\n", debugstr_w(desktop));
        return TRUE;
    }

    ret = EnumDesktopWindows( hdesktop, bcast_childwindow, lp );
    NtUserCloseDesktop( hdesktop );
    TRACE("-->%d\n", ret);
    return parm->success;
}

static BOOL CALLBACK bcast_winsta( LPWSTR winsta, LPARAM lp )
{
    BOOL ret;
    HWINSTA hwinsta = OpenWindowStationW( winsta, FALSE, WINSTA_ENUMDESKTOPS );
    TRACE("hwinsta: %p/%s/%08x\n", hwinsta, debugstr_w( winsta ), GetLastError());
    if (!hwinsta)
        return TRUE;
    ((BroadcastParm *)lp)->winsta = hwinsta;
    ret = EnumDesktopsW( hwinsta, bcast_desktop, lp );
    NtUserCloseWindowStation( hwinsta );
    TRACE("-->%d\n", ret);
    return ret;
}

/***********************************************************************
 *		BroadcastSystemMessageA (USER32.@)
 *		BroadcastSystemMessage  (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageA( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp )
{
    return BroadcastSystemMessageExA( flags, recipients, msg, wp, lp, NULL );
}


/***********************************************************************
 *		BroadcastSystemMessageW (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageW( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp )
{
    return BroadcastSystemMessageExW( flags, recipients, msg, wp, lp, NULL );
}

/***********************************************************************
 *              BroadcastSystemMessageExA (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageExA( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp, PBSMINFO pinfo )
{
    map_wparam_AtoW( msg, &wp, WMCHAR_MAP_NOMAPPING );
    return BroadcastSystemMessageExW( flags, recipients, msg, wp, lp, NULL );
}


/***********************************************************************
 *              BroadcastSystemMessageExW (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageExW( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp, PBSMINFO pinfo )
{
    BroadcastParm parm;
    DWORD recips = BSM_ALLCOMPONENTS;
    BOOL ret = TRUE;
    static const DWORD all_flags = ( BSF_QUERY | BSF_IGNORECURRENTTASK | BSF_FLUSHDISK | BSF_NOHANG
                                   | BSF_POSTMESSAGE | BSF_FORCEIFHUNG | BSF_NOTIMEOUTIFNOTHUNG
                                   | BSF_ALLOWSFW | BSF_SENDNOTIFYMESSAGE | BSF_RETURNHDESK | BSF_LUID );

    TRACE("Flags: %08x, recipients: %p(0x%x), msg: %04x, wparam: %08lx, lparam: %08lx\n", flags, recipients,
         (recipients ? *recipients : recips), msg, wp, lp);

    if (flags & ~all_flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!recipients)
        recipients = &recips;

    if ( pinfo && flags & BSF_QUERY )
        FIXME("Not returning PBSMINFO information yet\n");

    parm.flags = flags;
    parm.recipients = recipients;
    parm.msg = msg;
    parm.wp = wp;
    parm.lp = lp;
    parm.success = TRUE;

    if (*recipients & BSM_ALLDESKTOPS || *recipients == BSM_ALLCOMPONENTS)
        ret = EnumWindowStationsW(bcast_winsta, (LONG_PTR)&parm);
    else if (*recipients & BSM_APPLICATIONS)
    {
        EnumWindows(bcast_childwindow, (LONG_PTR)&parm);
        ret = parm.success;
    }
    else
        FIXME("Recipients %08x not supported!\n", *recipients);

    return ret;
}

/***********************************************************************
 *		SetMessageQueue (USER32.@)
 */
BOOL WINAPI SetMessageQueue( INT size )
{
    /* now obsolete the message queue will be expanded dynamically as necessary */
    return TRUE;
}


/***********************************************************************
 *		MessageBeep (USER32.@)
 */
BOOL WINAPI MessageBeep( UINT i )
{
    return NtUserCallOneParam( i, NtUserMessageBeep );
}


/******************************************************************
 *      SetTimer (USER32.@)
 */
UINT_PTR WINAPI SetTimer( HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc )
{
    return NtUserSetTimer( hwnd, id, timeout, proc, TIMERV_DEFAULT_COALESCING );
}


/***********************************************************************
 *		KillSystemTimer (USER32.@)
 */
BOOL WINAPI KillSystemTimer( HWND hwnd, UINT_PTR id )
{
    return NtUserCallHwndParam( hwnd, id, NtUserKillSystemTimer );
}


/**********************************************************************
 *		IsGUIThread  (USER32.@)
 */
BOOL WINAPI IsGUIThread( BOOL convert )
{
    FIXME( "%u: stub\n", convert );
    return TRUE;
}


/******************************************************************
 *		IsHungAppWindow (USER32.@)
 *
 */
BOOL WINAPI IsHungAppWindow( HWND hWnd )
{
    BOOL ret;

    SERVER_START_REQ( is_window_hung )
    {
        req->win = wine_server_user_handle( hWnd );
        ret = !wine_server_call_err( req ) && reply->is_hung;
    }
    SERVER_END_REQ;
    return ret;
}

/******************************************************************
 *      ChangeWindowMessageFilter (USER32.@)
 */
BOOL WINAPI ChangeWindowMessageFilter( UINT message, DWORD flag )
{
    FIXME( "%x %08x\n", message, flag );
    return TRUE;
}

/******************************************************************
 *      ChangeWindowMessageFilterEx (USER32.@)
 */
BOOL WINAPI ChangeWindowMessageFilterEx( HWND hwnd, UINT message, DWORD action, CHANGEFILTERSTRUCT *changefilter )
{
    FIXME( "%p %x %d %p\n", hwnd, message, action, changefilter );
    return TRUE;
}
