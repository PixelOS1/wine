/*
 * CRTDLL wide-char functions
 *
 * Copyright 1999 Alexandre Julliard
 *
 * TODO:
 *   These functions are really necessary only if sizeof(WCHAR) != sizeof(wchar_t),
 *   otherwise we could use the libc functions directly.
 */

#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

#include "windef.h"
#include "crtdll.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(crtdll);

/*********************************************************************
 *           CRTDLL__wcsdup    (CRTDLL.320)
 */
LPWSTR __cdecl CRTDLL__wcsdup( LPCWSTR str )
{
    LPWSTR ret = NULL;
    if (str)
    {
        int size = (CRTDLL_wcslen(str) + 1) * sizeof(WCHAR);
        ret = CRTDLL_malloc( size );
        if (ret) memcpy( ret, str, size );
    }
    return ret;
}


/*********************************************************************
 *           CRTDLL__wcsicmp    (CRTDLL.321)
 */
INT __cdecl CRTDLL__wcsicmp( LPCWSTR str1, LPCWSTR str2 )
{
    while (*str1 && (CRTDLL_towupper(*str1) == CRTDLL_towupper(*str2)))
    {
        str1++;
        str2++;
    }
    return CRTDLL_towupper(*str1) - CRTDLL_towupper(*str2);
}


/*********************************************************************
 *           CRTDLL__wcsicoll    (CRTDLL.322)
 */
INT __cdecl CRTDLL__wcsicoll( LPCWSTR str1, LPCWSTR str2 )
{
    /* FIXME: handle collates */
    return CRTDLL__wcsicmp( str1, str2 );
}


/*********************************************************************
 *           CRTDLL__wcslwr    (CRTDLL.323)
 */
LPWSTR __cdecl CRTDLL__wcslwr( LPWSTR str )
{
    LPWSTR ret = str;
    for ( ; *str; str++) *str = CRTDLL_towlower(*str);
    return ret;
}


/*********************************************************************
 *           CRTDLL__wcsnicmp    (CRTDLL.324)
 */
INT __cdecl CRTDLL__wcsnicmp( LPCWSTR str1, LPCWSTR str2, INT n )
{
    if (!n) return 0;
    while ((--n > 0) && *str1 && (CRTDLL_towupper(*str1) == CRTDLL_towupper(*str2)))
    {
        str1++;
        str2++;
    }
    return CRTDLL_towupper(*str1) - CRTDLL_towupper(*str2);
}


/*********************************************************************
 *           CRTDLL__wcsnset    (CRTDLL.325)
 */
LPWSTR __cdecl CRTDLL__wcsnset( LPWSTR str, WCHAR c, INT n )
{
    LPWSTR ret = str;
    while ((n-- > 0) && *str) *str++ = c;
    return ret;
}


/*********************************************************************
 *           CRTDLL__wcsrev    (CRTDLL.326)
 */
LPWSTR __cdecl CRTDLL__wcsrev( LPWSTR str )
{
    LPWSTR ret = str;
    LPWSTR end = str + CRTDLL_wcslen(str) - 1;
    while (end > str)
    {
        WCHAR t = *end;
        *end--  = *str;
        *str++  = t;
    }
    return ret;
}


/*********************************************************************
 *           CRTDLL__wcsset    (CRTDLL.327)
 */
LPWSTR __cdecl CRTDLL__wcsset( LPWSTR str, WCHAR c )
{
    LPWSTR ret = str;
    while (*str) *str++ = c;
    return ret;
}


/*********************************************************************
 *           CRTDLL__wcsupr    (CRTDLL.328)
 */
LPWSTR __cdecl CRTDLL__wcsupr( LPWSTR str )
{
    LPWSTR ret = str;
    for ( ; *str; str++) *str = CRTDLL_towupper(*str);
    return ret;
}


/*********************************************************************
 *           CRTDLL_towlower    (CRTDLL.493)
 */
WCHAR __cdecl CRTDLL_towlower( WCHAR ch )
{
#ifdef HAVE_WCTYPE_H
    ch = (WCHAR)towlower( (wchar_t)ch );
#else
    if (!HIBYTE(ch)) ch = (WCHAR)tolower( LOBYTE(ch) );  /* FIXME */
#endif
    return ch;
}


/*********************************************************************
 *           CRTDLL_towupper    (CRTDLL.494)
 */
WCHAR __cdecl CRTDLL_towupper( WCHAR ch )
{
#ifdef HAVE_WCTYPE_H
    ch = (WCHAR)towupper( (wchar_t)ch );
#else
    if (!HIBYTE(ch)) ch = (WCHAR)toupper( LOBYTE(ch) );  /* FIXME */
#endif
    return ch;
}


/***********************************************************************
 *           CRTDLL_wcscat    (CRTDLL.503)
 */
LPWSTR __cdecl CRTDLL_wcscat( LPWSTR dst, LPCWSTR src )
{
    LPWSTR p = dst;
    while (*p) p++;
    while ((*p++ = *src++));
    return dst;
}


/*********************************************************************
 *           CRTDLL_wcschr    (CRTDLL.504)
 */
LPWSTR __cdecl CRTDLL_wcschr( LPCWSTR str, WCHAR ch )
{
    while (*str)
    {
        if (*str == ch) return (LPWSTR)str;
        str++;
    }
    return NULL;
}


/*********************************************************************
 *           CRTDLL_wcscmp    (CRTDLL.505)
 */
INT __cdecl CRTDLL_wcscmp( LPCWSTR str1, LPCWSTR str2 )
{
    while (*str1 && (*str1 == *str2)) { str1++; str2++; }
    return (INT)(*str1 - *str2);
}


/*********************************************************************
 *           CRTDLL_wcscoll    (CRTDLL.506)
 */
DWORD __cdecl CRTDLL_wcscoll( LPCWSTR str1, LPCWSTR str2 )
{
    /* FIXME: handle collates */
    return CRTDLL_wcscmp( str1, str2 );
}


/***********************************************************************
 *           CRTDLL_wcscpy    (CRTDLL.507)
 */
LPWSTR __cdecl CRTDLL_wcscpy( LPWSTR dst, LPCWSTR src )
{
    LPWSTR p = dst;
    while ((*p++ = *src++));
    return dst;
}


/*********************************************************************
 *           CRTDLL_wcscspn    (CRTDLL.508)
 */
INT __cdecl CRTDLL_wcscspn( LPCWSTR str, LPCWSTR reject )
{
    LPCWSTR start = str;
    while (*str)
    {
        LPCWSTR p = reject;
        while (*p && (*p != *str)) p++;
        if (*p) break;
        str++;
    }
    return str - start;
}


/***********************************************************************
 *           CRTDLL_wcslen    (CRTDLL.510)
 */
INT __cdecl CRTDLL_wcslen( LPCWSTR str )
{
    LPCWSTR s = str;
    while (*s) s++;
    return s - str;
}


/*********************************************************************
 *           CRTDLL_wcsncat    (CRTDLL.511)
 */
LPWSTR __cdecl CRTDLL_wcsncat( LPWSTR s1, LPCWSTR s2, INT n )
{
    LPWSTR ret = s1;
    while (*s1) s1++;
    while (n-- > 0) if (!(*s1++ = *s2++)) return ret;
    *s1 = 0;
    return ret;
}


/*********************************************************************
 *           CRTDLL_wcsncmp    (CRTDLL.512)
 */
INT __cdecl CRTDLL_wcsncmp( LPCWSTR str1, LPCWSTR str2, INT n )
{
    if (!n) return 0;
    while ((--n > 0) && *str1 && (*str1 == *str2)) { str1++; str2++; }
    return (INT)(*str1 - *str2);
}


/*********************************************************************
 *           CRTDLL_wcsncpy    (CRTDLL.513)
 */
LPWSTR __cdecl CRTDLL_wcsncpy( LPWSTR s1, LPCWSTR s2, INT n )
{
    LPWSTR ret = s1;
    while (n-- > 0) if (!(*s1++ = *s2++)) break;
    while (n-- > 0) *s1++ = 0;
    return ret;
}


/*********************************************************************
 *           CRTDLL_wcspbrk    (CRTDLL.514)
 */
LPWSTR __cdecl CRTDLL_wcspbrk( LPCWSTR str, LPCWSTR accept )
{
    LPCWSTR p;
    while (*str)
    {
        for (p = accept; *p; p++) if (*p == *str) return (LPWSTR)str;
        str++;
    }
    return NULL;
}


/*********************************************************************
 *           CRTDLL_wcsrchr    (CRTDLL.515)
 */
LPWSTR __cdecl CRTDLL_wcsrchr( LPWSTR str, WCHAR ch )
{
    LPWSTR last = NULL;
    while (*str)
    {
        if (*str == ch) last = str;
        str++;
    }
    return last;
}


/*********************************************************************
 *           CRTDLL_wcsspn    (CRTDLL.516)
 */
INT __cdecl CRTDLL_wcsspn( LPCWSTR str, LPCWSTR accept )
{
    LPCWSTR start = str;
    while (*str)
    {
        LPCWSTR p = accept;
        while (*p && (*p != *str)) p++;
        if (!*p) break;
        str++;
    }
    return str - start;
}


/*********************************************************************
 *           CRTDLL_wcsstr    (CRTDLL.517)
 */
LPWSTR __cdecl CRTDLL_wcsstr( LPCWSTR str, LPCWSTR sub )
{
    while (*str)
    {
        LPCWSTR p1 = str, p2 = sub;
        while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
        if (!*p2) return (LPWSTR)str;
        str++;
    }
    return NULL;
}


/*********************************************************************
 *           CRTDLL_wcstok    (CRTDLL.519)
 */
LPWSTR __cdecl CRTDLL_wcstok( LPWSTR str, LPCWSTR delim )
{
    static LPWSTR next = NULL;
    LPWSTR ret;

    if (!str)
        if (!(str = next)) return NULL;

    while (*str && CRTDLL_wcschr( delim, *str )) str++;
    if (!*str) return NULL;
    ret = str++;
    while (*str && !CRTDLL_wcschr( delim, *str )) str++;
    if (*str) *str++ = 0;
    next = str;
    return ret;
}


/*********************************************************************
 *           CRTDLL_wcstombs    (CRTDLL.521)
 *
 * FIXME: the reason I do not use wcstombs is that it seems to fail
 *        for any latin-1 valid character. Not good.
 */
INT __cdecl CRTDLL_wcstombs( LPSTR dst, LPCWSTR src, INT n )
{
    int copied=0;
    while ((n>0) && *src) {
    	int ret;
	/* FIXME: could potentially overflow if we ever have MB of 2 bytes*/
	ret = wctomb(dst,(wchar_t)*src);
	if (ret<0) {
	    /* FIXME: sadly, some versions of glibc do not like latin characters
	     * as UNICODE chars for some reason (like german umlauts). Just
	     * copy those anyway. MM 991106
	     */
	    *dst=*src;
	    ret = 1;
	}
	dst	+= ret;
	n	-= ret;
	copied	+= ret;
	src++;
    }
    return copied;
}

/*********************************************************************
 *           CRTDLL_wctomb    (CRTDLL.524)
 */
INT __cdecl CRTDLL_wctomb( LPSTR dst, WCHAR ch )
{
    return wctomb( dst, (wchar_t)ch );
}

/*********************************************************************
 *           CRTDLL_iswalnum    (CRTDLL.405)
 */
int CRTDLL_iswalnum(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswalnum
    return iswalnum(wc);
#else
    return isalnum( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswalpha    (CRTDLL.406)
 */
int CRTDLL_iswalpha(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswalpha
    return iswalpha(wc);
#else
    return isalpha( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswcntrl    (CRTDLL.408)
 */
int CRTDLL_iswcntrl(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswcntrl
    return iswcntrl(wc);
#else
    return iscntrl( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswctype    (CRTDLL.409)
 */
int CRTDLL_iswctype(unsigned short wc, unsigned short wct)
{
#ifdef HAVE_WCTYPE_H
    wctype_t mask = 0;

    if (wct & 0x0001)	mask |= _ISwupper;
    if (wct & 0x0002)	mask |= _ISwlower;
    if (wct & 0x0004)	mask |= _ISwdigit;
    if (wct & 0x0008)	mask |= _ISwspace;
    if (wct & 0x0010)	mask |= _ISwpunct;
    if (wct & 0x0020)	mask |= _ISwcntrl;
    if (wct & 0x0040)	mask |= _ISwblank;
    if (wct & 0x0080)	mask |= _ISwxdigit;
    if (wct & 0x0100)	mask |= _ISwalpha;
    if (wct & 0x8000)
	FIXME(": iswctype(%04hx,_LEADBYTE|...) requested\n",wc);

#undef iswctype
    return iswctype(wc,mask);
#else
    FIXME(":(%ld,%p): iswctype() not supported\n",hand,x2);
#endif
}

/*********************************************************************
 *           CRTDLL_iswdigit    (CRTDLL.410)
 */
int CRTDLL_iswdigit(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswdigit
    return iswdigit(wc);
#else
    return isdigit( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswgraph    (CRTDLL.411)
 */
int CRTDLL_iswgraph(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswgraph
    return iswgraph(wc);
#else
    return isgraph( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswlower    (CRTDLL.412)
 */
int CRTDLL_iswlower(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswlower
    return iswlower(wc);
#else
    return islower( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswprint    (CRTDLL.413)
 */
int CRTDLL_iswprint(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswprint
    return iswprint(wc);
#else
    return isprint( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswpunct    (CRTDLL.414)
 */
int CRTDLL_iswpunct(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswpunct
    return iswpunct(wc);
#else
    return ispunct( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswspace    (CRTDLL.415)
 */
int CRTDLL_iswspace(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswspace
    return iswspace(wc);
#else
    return isspace( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswupper    (CRTDLL.416)
 */
int CRTDLL_iswupper(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswupper
    return iswupper(wc);
#else
    return isupper( LOBYTE(ch) );  /* FIXME */
#endif
}

/*********************************************************************
 *           CRTDLL_iswxdigit    (CRTDLL.417)
 */
int CRTDLL_iswxdigit(unsigned short wc)
{
#ifdef HAVE_WCTYPE_H
#undef iswxdigit
    return iswxdigit(wc);
#else
    return isxdigit( LOBYTE(ch) );  /* FIXME */
#endif
}
