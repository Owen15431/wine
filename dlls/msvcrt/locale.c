/*
 * msvcrt.dll locale functions
 *
 * Copyright 2000 Jon Griffiths
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

#include "config.h"
#include "wine/port.h"

#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"

#include "msvcrt.h"
#include "mtdll.h"

#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(msvcrt);

#define MAX_ELEM_LEN 64 /* Max length of country/language/CP string */
#define MAX_LOCALE_LENGTH 256
MSVCRT__locale_t MSVCRT_locale = NULL;
unsigned short *MSVCRT__pctype = NULL;
unsigned int MSVCRT___lc_codepage = 0;
int MSVCRT___lc_collate_cp = 0;
LCID MSVCRT___lc_handle[MSVCRT_LC_MAX - MSVCRT_LC_MIN + 1] = { 0 };
int MSVCRT___mb_cur_max = 1;
static unsigned char charmax = CHAR_MAX;

#define MSVCRT_LEADBYTE  0x8000
#define MSVCRT_C1_DEFINED 0x200

/* Friendly country strings & language names abbreviations. */
static const char * const _country_synonyms[] =
{
    "american", "enu",
    "american english", "enu",
    "american-english", "enu",
    "english-american", "enu",
    "english-us", "enu",
    "english-usa", "enu",
    "us", "enu",
    "usa", "enu",
    "australian", "ena",
    "english-aus", "ena",
    "belgian", "nlb",
    "french-belgian", "frb",
    "canadian", "enc",
    "english-can", "enc",
    "french-canadian", "frc",
    "chinese", "chs",
    "chinese-simplified", "chs",
    "chinese-traditional", "cht",
    "dutch-belgian", "nlb",
    "english-nz", "enz",
    "uk", "eng",
    "english-uk", "eng",
    "french-swiss", "frs",
    "swiss", "des",
    "german-swiss", "des",
    "italian-swiss", "its",
    "german-austrian", "dea",
    "portuguese", "ptb",
    "portuguese-brazil", "ptb",
    "spanish-mexican", "esm",
    "norwegian-bokmal", "nor",
    "norwegian-nynorsk", "non",
    "spanish-modern", "esn"
};

/* INTERNAL: Map a synonym to an ISO code */
static void remap_synonym(char *name)
{
  unsigned int i;
  for (i = 0; i < sizeof(_country_synonyms)/sizeof(char*); i += 2 )
  {
    if (!strcasecmp(_country_synonyms[i],name))
    {
      TRACE(":Mapping synonym %s to %s\n",name,_country_synonyms[i+1]);
      strcpy(name, _country_synonyms[i+1]);
      return;
    }
  }
}

/* Note: Flags are weighted in order of matching importance */
#define FOUND_LANGUAGE         0x4
#define FOUND_COUNTRY          0x2
#define FOUND_CODEPAGE         0x1

typedef struct {
  char search_language[MAX_ELEM_LEN];
  char search_country[MAX_ELEM_LEN];
  char search_codepage[MAX_ELEM_LEN];
  char found_codepage[MAX_ELEM_LEN];
  unsigned int match_flags;
  LANGID found_lang_id;
} locale_search_t;

#define CONTINUE_LOOKING TRUE
#define STOP_LOOKING     FALSE

/* INTERNAL: Get and compare locale info with a given string */
static int compare_info(LCID lcid, DWORD flags, char* buff, const char* cmp, BOOL exact)
{
  int len;

  if(!cmp[0])
      return 0;

  buff[0] = 0;
  GetLocaleInfoA(lcid, flags|LOCALE_NOUSEROVERRIDE, buff, MAX_ELEM_LEN);
  if (!buff[0])
    return 0;

  /* Partial matches are only allowed on language/country names */
  len = strlen(cmp);
  if(exact || len<=3)
    return !strcasecmp(cmp, buff);
  else
    return !strncasecmp(cmp, buff, len);
}

static BOOL CALLBACK
find_best_locale_proc(HMODULE hModule, LPCSTR type, LPCSTR name, WORD LangID, LONG_PTR lParam)
{
  locale_search_t *res = (locale_search_t *)lParam;
  const LCID lcid = MAKELCID(LangID, SORT_DEFAULT);
  char buff[MAX_ELEM_LEN];
  unsigned int flags = 0;

  if(PRIMARYLANGID(LangID) == LANG_NEUTRAL)
    return CONTINUE_LOOKING;

  /* Check Language */
  if (compare_info(lcid,LOCALE_SISO639LANGNAME,buff,res->search_language, TRUE) ||
      compare_info(lcid,LOCALE_SABBREVLANGNAME,buff,res->search_language, TRUE) ||
      compare_info(lcid,LOCALE_SENGLANGUAGE,buff,res->search_language, FALSE))
  {
    TRACE(":Found language: %s->%s\n", res->search_language, buff);
    flags |= FOUND_LANGUAGE;
  }
  else if (res->match_flags & FOUND_LANGUAGE)
  {
    return CONTINUE_LOOKING;
  }

  /* Check Country */
  if (compare_info(lcid,LOCALE_SISO3166CTRYNAME,buff,res->search_country, TRUE) ||
      compare_info(lcid,LOCALE_SABBREVCTRYNAME,buff,res->search_country, TRUE) ||
      compare_info(lcid,LOCALE_SENGCOUNTRY,buff,res->search_country, FALSE))
  {
    TRACE("Found country:%s->%s\n", res->search_country, buff);
    flags |= FOUND_COUNTRY;
  }
  else if (!flags && (res->match_flags & FOUND_COUNTRY))
  {
    return CONTINUE_LOOKING;
  }

  /* Check codepage */
  if (compare_info(lcid,LOCALE_IDEFAULTCODEPAGE,buff,res->search_codepage, TRUE) ||
      (compare_info(lcid,LOCALE_IDEFAULTANSICODEPAGE,buff,res->search_codepage, TRUE)))
  {
    TRACE("Found codepage:%s->%s\n", res->search_codepage, buff);
    flags |= FOUND_CODEPAGE;
    memcpy(res->found_codepage,res->search_codepage,MAX_ELEM_LEN);
  }
  else if (!flags && (res->match_flags & FOUND_CODEPAGE))
  {
    return CONTINUE_LOOKING;
  }

  if (flags > res->match_flags)
  {
    /* Found a better match than previously */
    res->match_flags = flags;
    res->found_lang_id = LangID;
  }
  if ((flags & (FOUND_LANGUAGE | FOUND_COUNTRY | FOUND_CODEPAGE)) ==
        (FOUND_LANGUAGE | FOUND_COUNTRY | FOUND_CODEPAGE))
  {
    TRACE(":found exact locale match\n");
    return STOP_LOOKING;
  }
  return CONTINUE_LOOKING;
}

extern int atoi(const char *);

/* Internal: Find the LCID for a locale specification */
LCID MSVCRT_locale_to_LCID(const char *locale, unsigned short *codepage)
{
    thread_data_t *data = msvcrt_get_thread_data();
    LCID lcid;
    locale_search_t search;
    const char *cp, *region;

    if (!strcmp(locale, data->cached_locale)) {
        if (codepage)
            *codepage = data->cached_cp;
        return data->cached_lcid;
    }

    memset(&search, 0, sizeof(locale_search_t));

    cp = strchr(locale, '.');
    region = strchr(locale, '_');

    lstrcpynA(search.search_language, locale, MAX_ELEM_LEN);
    if(region) {
        lstrcpynA(search.search_country, region+1, MAX_ELEM_LEN);
        if(region-locale < MAX_ELEM_LEN)
            search.search_language[region-locale] = '\0';
    } else
        search.search_country[0] = '\0';

    if(cp) {
        lstrcpynA(search.search_codepage, cp+1, MAX_ELEM_LEN);
        if(region && cp-region-1<MAX_ELEM_LEN)
          search.search_country[cp-region-1] = '\0';
        if(cp-locale < MAX_ELEM_LEN)
            search.search_language[cp-locale] = '\0';
    } else
        search.search_codepage[0] = '\0';

    if(!search.search_country[0] && !search.search_codepage[0])
        remap_synonym(search.search_language);

    if(!strcasecmp(search.search_country, "China"))
        strcpy(search.search_country, "People's Republic of China");

    EnumResourceLanguagesA(GetModuleHandleA("KERNEL32"), (LPSTR)RT_STRING,
            (LPCSTR)LOCALE_ILANGUAGE,find_best_locale_proc,
            (LONG_PTR)&search);

    if (!search.match_flags)
        return -1;

    /* If we were given something that didn't match, fail */
    if (search.search_country[0] && !(search.match_flags & FOUND_COUNTRY))
        return -1;

    lcid =  MAKELCID(search.found_lang_id, SORT_DEFAULT);

    /* Populate partial locale, translating LCID to locale string elements */
    if (!(search.match_flags & FOUND_CODEPAGE)) {
        /* Even if a codepage is not enumerated for a locale
         * it can be set if valid */
        if (search.search_codepage[0]) {
            if (IsValidCodePage(atoi(search.search_codepage)))
                memcpy(search.found_codepage,search.search_codepage,MAX_ELEM_LEN);
            else {
                /* Special codepage values: OEM & ANSI */
                if (!strcasecmp(search.search_codepage,"OCP")) {
                    GetLocaleInfoA(lcid, LOCALE_IDEFAULTCODEPAGE,
                            search.found_codepage, MAX_ELEM_LEN);
                } else if (!strcasecmp(search.search_codepage,"ACP")) {
                    GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE,
                            search.found_codepage, MAX_ELEM_LEN);
                } else
                    return -1;

                if (!atoi(search.found_codepage))
                    return -1;
            }
        } else {
            /* Prefer ANSI codepages if present */
            GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE,
                    search.found_codepage, MAX_ELEM_LEN);
            if (!search.found_codepage[0] || !atoi(search.found_codepage))
                GetLocaleInfoA(lcid, LOCALE_IDEFAULTCODEPAGE,
                        search.found_codepage, MAX_ELEM_LEN);
        }
    }
    if (codepage)
        *codepage = atoi(search.found_codepage);

    if (strlen(locale) < sizeof(data->cached_locale)) {
        strcpy(data->cached_locale, locale);
        data->cached_lcid = lcid;
        data->cached_cp = codepage ? *codepage : atoi(search.found_codepage);
    }

    return lcid;
}

/* INTERNAL: Set lc_handle, lc_id and lc_category in threadlocinfo struct */
static BOOL update_threadlocinfo_category(LCID lcid, unsigned short cp,
        MSVCRT_pthreadlocinfo locinfo, int category)
{
    char buf[256], *p;
    int len;

    if(GetLocaleInfoA(lcid, LOCALE_ILANGUAGE|LOCALE_NOUSEROVERRIDE, buf, 256)) {
        p = buf;

        locinfo->lc_id[category].wLanguage = 0;
        while(*p) {
            locinfo->lc_id[category].wLanguage *= 16;

            if(*p <= '9')
                locinfo->lc_id[category].wLanguage += *p-'0';
            else
                locinfo->lc_id[category].wLanguage += *p-'a'+10;

            p++;
        }

        locinfo->lc_id[category].wCountry =
            locinfo->lc_id[category].wLanguage;
    }

    locinfo->lc_id[category].wCodePage = cp;

    locinfo->lc_handle[category] = lcid;

    len = 0;
    len += GetLocaleInfoA(lcid, LOCALE_SENGLANGUAGE
            |LOCALE_NOUSEROVERRIDE, buf, 256);
    buf[len-1] = '_';
    len += GetLocaleInfoA(lcid, LOCALE_SENGCOUNTRY
            |LOCALE_NOUSEROVERRIDE, &buf[len], 256-len);
    buf[len-1] = '.';
    sprintf(buf+len, "%d", cp);
    len += strlen(buf+len)+1;

    locinfo->lc_category[category].locale = MSVCRT_malloc(len);
    locinfo->lc_category[category].refcount = MSVCRT_malloc(sizeof(int));
    if(!locinfo->lc_category[category].locale
            || !locinfo->lc_category[category].refcount) {
        MSVCRT_free(locinfo->lc_category[category].locale);
        MSVCRT_free(locinfo->lc_category[category].refcount);
        locinfo->lc_category[category].locale = NULL;
        locinfo->lc_category[category].refcount = NULL;
        return TRUE;
    }
    memcpy(locinfo->lc_category[category].locale, buf, len);
    *locinfo->lc_category[category].refcount = 1;

    return FALSE;
}

/* INTERNAL: swap pointers values */
static inline void swap_pointers(void **p1, void **p2) {
    void *hlp;

    hlp = *p1;
    *p1 = *p2;
    *p2 = hlp;
}

/* INTERNAL: returns pthreadlocinfo struct */
MSVCRT_pthreadlocinfo get_locinfo(void) {
    thread_data_t *data = msvcrt_get_thread_data();

    if(!data || !data->have_locale)
        return MSVCRT_locale->locinfo;

    return data->locinfo;
}

/* INTERNAL: returns pthreadlocinfo struct */
MSVCRT_pthreadmbcinfo get_mbcinfo(void) {
    thread_data_t *data = msvcrt_get_thread_data();

    if(!data || !data->have_locale)
        return MSVCRT_locale->mbcinfo;

    return data->mbcinfo;
}

/* INTERNAL: constructs string returned by setlocale */
static inline char* construct_lc_all(MSVCRT_pthreadlocinfo locinfo) {
    static char current_lc_all[MAX_LOCALE_LENGTH];

    int i;

    for(i=MSVCRT_LC_MIN+1; i<MSVCRT_LC_MAX; i++) {
        if(strcmp(locinfo->lc_category[i].locale,
                    locinfo->lc_category[i+1].locale))
            break;
    }

    if(i==MSVCRT_LC_MAX)
        return locinfo->lc_category[MSVCRT_LC_COLLATE].locale;

    sprintf(current_lc_all,
            "LC_COLLATE=%s;LC_CTYPE=%s;LC_MONETARY=%s;LC_NUMERIC=%s;LC_TIME=%s",
            locinfo->lc_category[MSVCRT_LC_COLLATE].locale,
            locinfo->lc_category[MSVCRT_LC_CTYPE].locale,
            locinfo->lc_category[MSVCRT_LC_MONETARY].locale,
            locinfo->lc_category[MSVCRT_LC_NUMERIC].locale,
            locinfo->lc_category[MSVCRT_LC_TIME].locale);

    return current_lc_all;
}


/*********************************************************************
 *		_Getdays (MSVCRT.@)
 */
char* CDECL _Getdays(void)
{
    MSVCRT___lc_time_data *cur = get_locinfo()->lc_time_curr;
    int i, len, size;
    char *out;

    TRACE("\n");

    size = cur->str.names.short_mon[0]-cur->str.names.short_wday[0];
    out = MSVCRT_malloc(size+1);
    if(!out)
        return NULL;

    size = 0;
    for(i=0; i<7; i++) {
        out[size++] = ':';
        len = strlen(cur->str.names.short_wday[i]);
        memcpy(&out[size], cur->str.names.short_wday[i], len);
        size += len;

        out[size++] = ':';
        len = strlen(cur->str.names.wday[i]);
        memcpy(&out[size], cur->str.names.wday[i], len);
        size += len;
    }
    out[size] = '\0';

    return out;
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *		_W_Getdays (MSVCR110.@)
 */
MSVCRT_wchar_t* CDECL _W_Getdays(void)
{
    MSVCRT___lc_time_data *cur = get_locinfo()->lc_time_curr;
    MSVCRT_wchar_t *out;
    int i, len, size;

    TRACE("\n");

    size = cur->wstr.names.short_mon[0]-cur->wstr.names.short_wday[0];
    out = MSVCRT_malloc((size+1)*sizeof(*out));
    if(!out)
        return NULL;

    size = 0;
    for(i=0; i<7; i++) {
        out[size++] = ':';
        len = strlenW(cur->wstr.names.short_wday[i]);
        memcpy(&out[size], cur->wstr.names.short_wday[i], len*sizeof(*out));
        size += len;

        out[size++] = ':';
        len = strlenW(cur->wstr.names.wday[i]);
        memcpy(&out[size], cur->wstr.names.wday[i], len*sizeof(*out));
        size += len;
    }
    out[size] = '\0';

    return out;
}
#endif

/*********************************************************************
 *		_Getmonths (MSVCRT.@)
 */
char* CDECL _Getmonths(void)
{
    MSVCRT___lc_time_data *cur = get_locinfo()->lc_time_curr;
    int i, len, size;
    char *out;

    TRACE("\n");

    size = cur->str.names.am-cur->str.names.short_mon[0];
    out = MSVCRT_malloc(size+1);
    if(!out)
        return NULL;

    size = 0;
    for(i=0; i<12; i++) {
        out[size++] = ':';
        len = strlen(cur->str.names.short_mon[i]);
        memcpy(&out[size], cur->str.names.short_mon[i], len);
        size += len;

        out[size++] = ':';
        len = strlen(cur->str.names.mon[i]);
        memcpy(&out[size], cur->str.names.mon[i], len);
        size += len;
    }
    out[size] = '\0';

    return out;
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *		_W_Getmonths (MSVCR110.@)
 */
MSVCRT_wchar_t* CDECL _W_Getmonths(void)
{
    MSVCRT___lc_time_data *cur = get_locinfo()->lc_time_curr;
    MSVCRT_wchar_t *out;
    int i, len, size;

    TRACE("\n");

    size = cur->wstr.names.am-cur->wstr.names.short_mon[0];
    out = MSVCRT_malloc((size+1)*sizeof(*out));
    if(!out)
        return NULL;

    size = 0;
    for(i=0; i<12; i++) {
        out[size++] = ':';
        len = strlenW(cur->wstr.names.short_mon[i]);
        memcpy(&out[size], cur->wstr.names.short_mon[i], len*sizeof(*out));
        size += len;

        out[size++] = ':';
        len = strlenW(cur->wstr.names.mon[i]);
        memcpy(&out[size], cur->wstr.names.mon[i], len*sizeof(*out));
        size += len;
    }
    out[size] = '\0';

    return out;
}
#endif

/*********************************************************************
 *		_Gettnames (MSVCRT.@)
 */
void* CDECL _Gettnames(void)
{
    MSVCRT___lc_time_data *ret, *cur = get_locinfo()->lc_time_curr;
    unsigned int i, size = sizeof(MSVCRT___lc_time_data);

    TRACE("\n");

    for(i=0; i<sizeof(cur->str.str)/sizeof(cur->str.str[0]); i++)
        size += strlen(cur->str.str[i])+1;

    ret = MSVCRT_malloc(size);
    if(!ret)
        return NULL;
    memcpy(ret, cur, size);

    size = 0;
    for(i=0; i<sizeof(cur->str.str)/sizeof(cur->str.str[0]); i++) {
        ret->str.str[i] = &ret->data[size];
        size += strlen(&ret->data[size])+1;
    }

    return ret;
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *              _W_Gettnames (MSVCR110.@)
 */
void* CDECL _W_Gettnames(void)
{
    return _Gettnames();
}
#endif

/*********************************************************************
 *		__crtLCMapStringA (MSVCRT.@)
 */
int CDECL __crtLCMapStringA(
  LCID lcid, DWORD mapflags, const char* src, int srclen, char* dst,
  int dstlen, unsigned int codepage, int xflag
) {
  FIXME("(lcid %x, flags %x, %s(%d), %p(%d), %x, %d), partial stub!\n",
        lcid,mapflags,src,srclen,dst,dstlen,codepage,xflag);
  /* FIXME: A bit incorrect. But msvcrt itself just converts its
   * arguments to wide strings and then calls LCMapStringW
   */
  return LCMapStringA(lcid,mapflags,src,srclen,dst,dstlen);
}

/*********************************************************************
 *              __crtLCMapStringW (MSVCRT.@)
 */
int CDECL __crtLCMapStringW(LCID lcid, DWORD mapflags, const MSVCRT_wchar_t *src,
        int srclen, MSVCRT_wchar_t *dst, int dstlen, unsigned int codepage, int xflag)
{
    FIXME("(lcid %x, flags %x, %s(%d), %p(%d), %x, %d), partial stub!\n",
            lcid, mapflags, debugstr_w(src), srclen, dst, dstlen, codepage, xflag);

    return LCMapStringW(lcid, mapflags, src, srclen, dst, dstlen);
}

/*********************************************************************
 *		__crtCompareStringA (MSVCRT.@)
 */
int CDECL __crtCompareStringA( LCID lcid, DWORD flags, const char *src1, int len1,
                               const char *src2, int len2 )
{
    FIXME("(lcid %x, flags %x, %s(%d), %s(%d), partial stub\n",
          lcid, flags, debugstr_a(src1), len1, debugstr_a(src2), len2 );
    /* FIXME: probably not entirely right */
    return CompareStringA( lcid, flags, src1, len1, src2, len2 );
}

/*********************************************************************
 *		__crtCompareStringW (MSVCRT.@)
 */
int CDECL __crtCompareStringW( LCID lcid, DWORD flags, const MSVCRT_wchar_t *src1, int len1,
                               const MSVCRT_wchar_t *src2, int len2 )
{
    FIXME("(lcid %x, flags %x, %s(%d), %s(%d), partial stub\n",
          lcid, flags, debugstr_w(src1), len1, debugstr_w(src2), len2 );
    /* FIXME: probably not entirely right */
    return CompareStringW( lcid, flags, src1, len1, src2, len2 );
}

/*********************************************************************
 *		__crtGetLocaleInfoW (MSVCRT.@)
 */
int CDECL __crtGetLocaleInfoW( LCID lcid, LCTYPE type, MSVCRT_wchar_t *buffer, int len )
{
    FIXME("(lcid %x, type %x, %p(%d), partial stub\n", lcid, type, buffer, len );
    /* FIXME: probably not entirely right */
    return GetLocaleInfoW( lcid, type, buffer, len );
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *		__crtGetLocaleInfoEx (MSVC110.@)
 */
int CDECL __crtGetLocaleInfoEx( const WCHAR *locale, LCTYPE type, MSVCRT_wchar_t *buffer, int len )
{
    TRACE("(%s, %x, %p, %d)\n", debugstr_w(locale), type, buffer, len);
    return GetLocaleInfoEx(locale, type, buffer, len);
}
#endif

/*********************************************************************
 *              btowc(MSVCRT.@)
 */
MSVCRT_wint_t CDECL MSVCRT_btowc(int c)
{
    unsigned char letter = c;
    MSVCRT_wchar_t ret;

    if(!MultiByteToWideChar(get_locinfo()->lc_handle[MSVCRT_LC_CTYPE],
                0, (LPCSTR)&letter, 1, &ret, 1))
        return 0;

    return ret;
}

/*********************************************************************
 *              __crtGetStringTypeW(MSVCRT.@)
 *
 * This function was accepting different number of arguments in older
 * versions of msvcrt.
 */
BOOL CDECL __crtGetStringTypeW(DWORD unk, DWORD type,
        MSVCRT_wchar_t *buffer, int len, WORD *out)
{
    FIXME("(unk %x, type %x, wstr %p(%d), %p) partial stub\n",
            unk, type, buffer, len, out);

    return GetStringTypeW(type, buffer, len, out);
}

/*********************************************************************
 *		localeconv (MSVCRT.@)
 */
struct MSVCRT_lconv * CDECL MSVCRT_localeconv(void)
{
    return get_locinfo()->lconv;
}

/*********************************************************************
 *		__lconv_init (MSVCRT.@)
 */
int CDECL __lconv_init(void)
{
    /* this is used to make chars unsigned */
    charmax = 255;
    return 0;
}

/*********************************************************************
 *      ___lc_handle_func (MSVCRT.@)
 */
LCID* CDECL ___lc_handle_func(void)
{
    return get_locinfo()->lc_handle;
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *      ___lc_locale_name_func (MSVCR110.@)
 */
MSVCRT_wchar_t** CDECL ___lc_locale_name_func(void)
{
    return get_locinfo()->lc_name;
}
#endif

/*********************************************************************
 *      ___lc_codepage_func (MSVCRT.@)
 */
unsigned int CDECL ___lc_codepage_func(void)
{
    return get_locinfo()->lc_codepage;
}

/*********************************************************************
 *      ___lc_collate_cp_func (MSVCRT.@)
 */
int CDECL ___lc_collate_cp_func(void)
{
    return get_locinfo()->lc_collate_cp;
}

/* INTERNAL: frees MSVCRT_pthreadlocinfo struct */
void free_locinfo(MSVCRT_pthreadlocinfo locinfo)
{
    int i;

    if(!locinfo)
        return;

    if(InterlockedDecrement(&locinfo->refcount))
        return;

    for(i=MSVCRT_LC_MIN+1; i<=MSVCRT_LC_MAX; i++) {
        MSVCRT_free(locinfo->lc_category[i].locale);
        MSVCRT_free(locinfo->lc_category[i].refcount);
#if _MSVCR_VER >= 110
        MSVCRT_free(locinfo->lc_name[i]);
#endif
    }

    if(locinfo->lconv) {
        MSVCRT_free(locinfo->lconv->decimal_point);
        MSVCRT_free(locinfo->lconv->thousands_sep);
        MSVCRT_free(locinfo->lconv->grouping);
        MSVCRT_free(locinfo->lconv->int_curr_symbol);
        MSVCRT_free(locinfo->lconv->currency_symbol);
        MSVCRT_free(locinfo->lconv->mon_decimal_point);
        MSVCRT_free(locinfo->lconv->mon_thousands_sep);
        MSVCRT_free(locinfo->lconv->mon_grouping);
        MSVCRT_free(locinfo->lconv->positive_sign);
        MSVCRT_free(locinfo->lconv->negative_sign);
#if _MSVCR_VER >= 100
        MSVCRT_free(locinfo->lconv->_W_decimal_point);
        MSVCRT_free(locinfo->lconv->_W_thousands_sep);
        MSVCRT_free(locinfo->lconv->_W_int_curr_symbol);
        MSVCRT_free(locinfo->lconv->_W_currency_symbol);
        MSVCRT_free(locinfo->lconv->_W_mon_decimal_point);
        MSVCRT_free(locinfo->lconv->_W_mon_thousands_sep);
        MSVCRT_free(locinfo->lconv->_W_positive_sign);
        MSVCRT_free(locinfo->lconv->_W_negative_sign);
#endif
    }
    MSVCRT_free(locinfo->lconv_intl_refcount);
    MSVCRT_free(locinfo->lconv_num_refcount);
    MSVCRT_free(locinfo->lconv_mon_refcount);
    MSVCRT_free(locinfo->lconv);

    MSVCRT_free(locinfo->ctype1_refcount);
    MSVCRT_free(locinfo->ctype1);

    MSVCRT_free(locinfo->pclmap);
    MSVCRT_free(locinfo->pcumap);

    MSVCRT_free(locinfo->lc_time_curr);

    MSVCRT_free(locinfo);
}

/* INTERNAL: frees MSVCRT_pthreadmbcinfo struct */
void free_mbcinfo(MSVCRT_pthreadmbcinfo mbcinfo)
{
    if(!mbcinfo)
        return;

    if(InterlockedDecrement(&mbcinfo->refcount))
        return;

    MSVCRT_free(mbcinfo);
}

/*********************************************************************
 *      _get_current_locale (MSVCRT.@)
 */
MSVCRT__locale_t CDECL MSVCRT__get_current_locale(void)
{
    MSVCRT__locale_t loc = MSVCRT_malloc(sizeof(MSVCRT__locale_tstruct));
    if(!loc)
        return NULL;

    loc->locinfo = get_locinfo();
    loc->mbcinfo = get_mbcinfo();
    InterlockedIncrement(&loc->locinfo->refcount);
    InterlockedIncrement(&loc->mbcinfo->refcount);
    return loc;
}

/*********************************************************************
 *      _free_locale (MSVCRT.@)
 */
void CDECL MSVCRT__free_locale(MSVCRT__locale_t locale)
{
    if (!locale)
        return;

    free_locinfo(locale->locinfo);
    free_mbcinfo(locale->mbcinfo);
    MSVCRT_free(locale);
}

#if _MSVCR_VER >= 110
static inline BOOL set_lc_locale_name(MSVCRT_pthreadlocinfo locinfo, int cat)
{
    LCID lcid = locinfo->lc_handle[cat];
    WCHAR buf[100];
    int len;

    len = GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME
            |LOCALE_NOUSEROVERRIDE, buf, 100);
    if(!len) return FALSE;

    if(LocaleNameToLCID(buf, 0) != lcid)
        len = LCIDToLocaleName(lcid, buf, 100, 0);

    if(!len || !(locinfo->lc_name[cat] = MSVCRT_malloc(len*sizeof(MSVCRT_wchar_t))))
        return FALSE;

    memcpy(locinfo->lc_name[cat], buf, len*sizeof(MSVCRT_wchar_t));
    return TRUE;
}
#else
static inline BOOL set_lc_locale_name(MSVCRT_pthreadlocinfo locinfo, int cat)
{
    return TRUE;
}
#endif

static inline BOOL category_needs_update(int cat, int user_cat,
        MSVCRT_pthreadlocinfo locinfo, LCID lcid, unsigned short cp)
{
    if(!locinfo) return TRUE;
    if(user_cat!=cat && user_cat!=MSVCRT_LC_ALL) return FALSE;
    return lcid!=locinfo->lc_handle[cat] || cp!=locinfo->lc_id[cat].wCodePage;
}

static MSVCRT_pthreadlocinfo create_locinfo(int category,
        const char *locale, MSVCRT_pthreadlocinfo old_locinfo)
{
    static const DWORD time_data[] = {
        LOCALE_SABBREVDAYNAME7, LOCALE_SABBREVDAYNAME1, LOCALE_SABBREVDAYNAME2,
        LOCALE_SABBREVDAYNAME3, LOCALE_SABBREVDAYNAME4, LOCALE_SABBREVDAYNAME5,
        LOCALE_SABBREVDAYNAME6,
        LOCALE_SDAYNAME7, LOCALE_SDAYNAME1, LOCALE_SDAYNAME2, LOCALE_SDAYNAME3,
        LOCALE_SDAYNAME4, LOCALE_SDAYNAME5, LOCALE_SDAYNAME6,
        LOCALE_SABBREVMONTHNAME1, LOCALE_SABBREVMONTHNAME2, LOCALE_SABBREVMONTHNAME3,
        LOCALE_SABBREVMONTHNAME4, LOCALE_SABBREVMONTHNAME5, LOCALE_SABBREVMONTHNAME6,
        LOCALE_SABBREVMONTHNAME7, LOCALE_SABBREVMONTHNAME8, LOCALE_SABBREVMONTHNAME9,
        LOCALE_SABBREVMONTHNAME10, LOCALE_SABBREVMONTHNAME11, LOCALE_SABBREVMONTHNAME12,
        LOCALE_SMONTHNAME1, LOCALE_SMONTHNAME2, LOCALE_SMONTHNAME3, LOCALE_SMONTHNAME4,
        LOCALE_SMONTHNAME5, LOCALE_SMONTHNAME6, LOCALE_SMONTHNAME7, LOCALE_SMONTHNAME8,
        LOCALE_SMONTHNAME9, LOCALE_SMONTHNAME10, LOCALE_SMONTHNAME11, LOCALE_SMONTHNAME12,
        LOCALE_S1159, LOCALE_S2359,
        LOCALE_SSHORTDATE, LOCALE_SLONGDATE,
        LOCALE_STIMEFORMAT
    };
    static const char collate[] = "COLLATE=";
    static const char ctype[] = "CTYPE=";
    static const char monetary[] = "MONETARY=";
    static const char numeric[] = "NUMERIC=";
    static const char time[] = "TIME=";
    static const char cloc_short_date[] = "MM/dd/yy";
    static const MSVCRT_wchar_t cloc_short_dateW[] = {'M','M','/','d','d','/','y','y',0};
    static const char cloc_long_date[] = "dddd, MMMM dd, yyyy";
    static const MSVCRT_wchar_t cloc_long_dateW[] = {'d','d','d','d',',',' ','M','M','M','M',' ','d','d',',',' ','y','y','y','y',0};
    static const char cloc_time[] = "HH:mm:ss";
    static const MSVCRT_wchar_t cloc_timeW[] = {'H','H',':','m','m',':','s','s',0};

    MSVCRT_pthreadlocinfo locinfo;
    LCID lcid[6] = { 0 }, lcid_tmp;
    unsigned short cp[6] = { 0 };
    char buf[256];
#if _MSVCR_VER >= 100
    MSVCRT_wchar_t wbuf[256];
#endif
    int i, ret, size;

    TRACE("(%d %s)\n", category, locale);

    if(category<MSVCRT_LC_MIN || category>MSVCRT_LC_MAX || !locale)
        return NULL;

    if(locale[0]=='C' && !locale[1]) {
        lcid[0] = 0;
        cp[0] = CP_ACP;
    } else if(!locale[0]) {
        lcid[0] = GetSystemDefaultLCID();
        GetLocaleInfoA(lcid[0], LOCALE_IDEFAULTANSICODEPAGE
                |LOCALE_NOUSEROVERRIDE, buf, sizeof(buf));
        cp[0] = atoi(buf);

        for(i=1; i<6; i++) {
            lcid[i] = lcid[0];
            cp[i] = cp[0];
        }
    } else if (locale[0] == 'L' && locale[1] == 'C' && locale[2] == '_') {
        const char *p;

        while(1) {
            locale += 3; /* LC_ */
            if(!memcmp(locale, collate, sizeof(collate)-1)) {
                i = MSVCRT_LC_COLLATE;
                locale += sizeof(collate)-1;
            } else if(!memcmp(locale, ctype, sizeof(ctype)-1)) {
                i = MSVCRT_LC_CTYPE;
                locale += sizeof(ctype)-1;
            } else if(!memcmp(locale, monetary, sizeof(monetary)-1)) {
                i = MSVCRT_LC_MONETARY;
                locale += sizeof(monetary)-1;
            } else if(!memcmp(locale, numeric, sizeof(numeric)-1)) {
                i = MSVCRT_LC_NUMERIC;
                locale += sizeof(numeric)-1;
            } else if(!memcmp(locale, time, sizeof(time)-1)) {
                i = MSVCRT_LC_TIME;
                locale += sizeof(time)-1;
            } else
                return NULL;

            p = strchr(locale, ';');
            if(locale[0]=='C' && (locale[1]==';' || locale[1]=='\0')) {
                lcid[i] = 0;
                cp[i] = CP_ACP;
            } else if(p) {
                memcpy(buf, locale, p-locale);
                buf[p-locale] = '\0';
                lcid[i] = MSVCRT_locale_to_LCID(buf, &cp[i]);
            } else
                lcid[i] = MSVCRT_locale_to_LCID(locale, &cp[i]);

            if(lcid[i] == -1)
                return NULL;

            if(!p || *(p+1)!='L' || *(p+2)!='C' || *(p+3)!='_')
                break;

            locale = p+1;
        }
    } else {
        lcid[0] = MSVCRT_locale_to_LCID(locale, &cp[0]);
        if(lcid[0] == -1)
            return NULL;

        for(i=1; i<6; i++) {
            lcid[i] = lcid[0];
            cp[i] = cp[0];
        }
    }

    locinfo = MSVCRT_malloc(sizeof(MSVCRT_threadlocinfo));
    if(!locinfo)
        return NULL;

    memset(locinfo, 0, sizeof(MSVCRT_threadlocinfo));
    locinfo->refcount = 1;

    locinfo->lconv = MSVCRT_malloc(sizeof(struct MSVCRT_lconv));
    if(!locinfo->lconv) {
        free_locinfo(locinfo);
        return NULL;
    }
    memset(locinfo->lconv, 0, sizeof(struct MSVCRT_lconv));

    locinfo->pclmap = MSVCRT_malloc(sizeof(char[256]));
    locinfo->pcumap = MSVCRT_malloc(sizeof(char[256]));
    if(!locinfo->pclmap || !locinfo->pcumap) {
        free_locinfo(locinfo);
        return NULL;
    }

    if(!category_needs_update(MSVCRT_LC_COLLATE, category, old_locinfo,
                lcid[MSVCRT_LC_COLLATE], cp[MSVCRT_LC_COLLATE])) {
        locinfo->lc_handle[MSVCRT_LC_COLLATE] = old_locinfo->lc_handle[MSVCRT_LC_COLLATE];
        locinfo->lc_id[MSVCRT_LC_COLLATE].wCodePage = old_locinfo->lc_id[MSVCRT_LC_COLLATE].wCodePage;
    } else if(lcid[MSVCRT_LC_COLLATE] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_COLLATE)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_COLLATE],
                    cp[MSVCRT_LC_COLLATE], locinfo, MSVCRT_LC_COLLATE)) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lc_collate_cp = locinfo->lc_id[MSVCRT_LC_COLLATE].wCodePage;

        if(!set_lc_locale_name(locinfo, MSVCRT_LC_COLLATE)) {
            free_locinfo(locinfo);
            return NULL;
        }
    } else
        locinfo->lc_category[MSVCRT_LC_COLLATE].locale = MSVCRT__strdup("C");

    if(!category_needs_update(MSVCRT_LC_CTYPE, category, old_locinfo,
                lcid[MSVCRT_LC_CTYPE], cp[MSVCRT_LC_CTYPE])) {
        locinfo->lc_handle[MSVCRT_LC_CTYPE] = old_locinfo->lc_handle[MSVCRT_LC_CTYPE];
        locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage = old_locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage;
    } else if(lcid[MSVCRT_LC_CTYPE] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_CTYPE)) {
        CPINFO cp_info;
        int j;

        if(update_threadlocinfo_category(lcid[MSVCRT_LC_CTYPE],
                    cp[MSVCRT_LC_CTYPE], locinfo, MSVCRT_LC_CTYPE)) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lc_codepage = locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage;
        locinfo->lc_clike = 1;
        if(!GetCPInfo(locinfo->lc_codepage, &cp_info)) {
            free_locinfo(locinfo);
            return NULL;
        }
        locinfo->mb_cur_max = cp_info.MaxCharSize;

        locinfo->ctype1_refcount = MSVCRT_malloc(sizeof(int));
        locinfo->ctype1 = MSVCRT_malloc(sizeof(short[257]));
        if(!locinfo->ctype1_refcount || !locinfo->ctype1) {
            free_locinfo(locinfo);
            return NULL;
        }

        *locinfo->ctype1_refcount = 1;
        locinfo->ctype1[0] = 0;
        locinfo->pctype = locinfo->ctype1+1;

        buf[1] = buf[2] = '\0';
        for(i=1; i<257; i++) {
            buf[0] = i-1;

            /* builtin GetStringTypeA doesn't set output to 0 on invalid input */
            locinfo->ctype1[i] = 0;

            GetStringTypeA(lcid[MSVCRT_LC_CTYPE], CT_CTYPE1, buf,
                    1, locinfo->ctype1+i);
        }

        for(i=0; cp_info.LeadByte[i+1]!=0; i+=2)
            for(j=cp_info.LeadByte[i]; j<=cp_info.LeadByte[i+1]; j++)
                locinfo->ctype1[j+1] |= MSVCRT__LEADBYTE;

        if(!set_lc_locale_name(locinfo, MSVCRT_LC_CTYPE)) {
            free_locinfo(locinfo);
            return NULL;
        }

        for(i=0; i<256; i++) {
            if(locinfo->pctype[i] & MSVCRT__LEADBYTE)
                buf[i] = ' ';
            else
                buf[i] = i;
        }

        LCMapStringA(lcid[MSVCRT_LC_CTYPE], LCMAP_LOWERCASE, buf, 256,
                (char*)locinfo->pclmap, 256);
        LCMapStringA(lcid[MSVCRT_LC_CTYPE], LCMAP_UPPERCASE, buf, 256,
                (char*)locinfo->pcumap, 256);
    } else {
        locinfo->lc_clike = 1;
        locinfo->mb_cur_max = 1;
        locinfo->pctype = MSVCRT__ctype+1;
        locinfo->lc_category[MSVCRT_LC_CTYPE].locale = MSVCRT__strdup("C");

        for(i=0; i<256; i++) {
            if(locinfo->pctype[i] & MSVCRT__LEADBYTE)
                buf[i] = ' ';
            else
                buf[i] = i;
        }

        for(i=0; i<256; i++) {
            locinfo->pclmap[i] = (i>='A' && i<='Z' ? i-'A'+'a' : i);
            locinfo->pcumap[i] = (i>='a' && i<='z' ? i-'a'+'A' : i);
        }
    }

    if(!category_needs_update(MSVCRT_LC_MONETARY, category, old_locinfo,
                lcid[MSVCRT_LC_MONETARY], cp[MSVCRT_LC_MONETARY])) {
        locinfo->lc_handle[MSVCRT_LC_MONETARY] = old_locinfo->lc_handle[MSVCRT_LC_MONETARY];
        locinfo->lc_id[MSVCRT_LC_MONETARY].wCodePage = old_locinfo->lc_id[MSVCRT_LC_MONETARY].wCodePage;
    } else if(lcid[MSVCRT_LC_MONETARY] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_MONETARY)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_MONETARY],
                    cp[MSVCRT_LC_MONETARY], locinfo, MSVCRT_LC_MONETARY)) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lconv_intl_refcount = MSVCRT_malloc(sizeof(int));
        locinfo->lconv_mon_refcount = MSVCRT_malloc(sizeof(int));
        if(!locinfo->lconv_intl_refcount || !locinfo->lconv_mon_refcount) {
            free_locinfo(locinfo);
            return NULL;
        }

        *locinfo->lconv_intl_refcount = 1;
        *locinfo->lconv_mon_refcount = 1;

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SINTLSYMBOL
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->int_curr_symbol = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->int_curr_symbol, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SCURRENCY
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->currency_symbol = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->currency_symbol, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONDECIMALSEP
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->mon_decimal_point = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->mon_decimal_point, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONTHOUSANDSEP
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->mon_thousands_sep = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->mon_thousands_sep, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONGROUPING
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i>1)
            i = i/2 + (buf[i-2]=='0'?0:1);
        if(i && (locinfo->lconv->mon_grouping = MSVCRT_malloc(i))) {
            for(i=0; buf[i+1]==';'; i+=2)
                locinfo->lconv->mon_grouping[i/2] = buf[i]-'0';
            locinfo->lconv->mon_grouping[i/2] = buf[i]-'0';
            if(buf[i] != '0')
                locinfo->lconv->mon_grouping[i/2+1] = 127;
        } else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SPOSITIVESIGN
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->positive_sign = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->positive_sign, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_SNEGATIVESIGN
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->negative_sign = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->negative_sign, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IINTLCURRDIGITS
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->int_frac_digits = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_ICURRDIGITS
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->frac_digits = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSYMPRECEDES
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->p_cs_precedes = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSEPBYSPACE
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->p_sep_by_space = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSYMPRECEDES
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->n_cs_precedes = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSEPBYSPACE
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->n_sep_by_space = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_IPOSSIGNPOSN
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->p_sign_posn = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        if(GetLocaleInfoA(lcid[MSVCRT_LC_MONETARY], LOCALE_INEGSIGNPOSN
                    |LOCALE_NOUSEROVERRIDE, buf, 256))
            locinfo->lconv->n_sign_posn = atoi(buf);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

#if _MSVCR_VER >= 100
        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SINTLSYMBOL
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_int_curr_symbol = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_int_curr_symbol, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SCURRENCY
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_currency_symbol = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_currency_symbol, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONDECIMALSEP
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_mon_decimal_point = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_mon_decimal_point, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SMONTHOUSANDSEP
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_mon_thousands_sep = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_mon_thousands_sep, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SPOSITIVESIGN
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_positive_sign = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_positive_sign, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_MONETARY], LOCALE_SNEGATIVESIGN
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_negative_sign = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_negative_sign, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }
#endif

        if(!set_lc_locale_name(locinfo, MSVCRT_LC_MONETARY)) {
            free_locinfo(locinfo);
            return NULL;
        }
    } else {
        locinfo->lconv->int_curr_symbol = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->currency_symbol = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->mon_decimal_point = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->mon_thousands_sep = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->mon_grouping = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->positive_sign = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->negative_sign = MSVCRT_malloc(sizeof(char));

        if(!locinfo->lconv->int_curr_symbol || !locinfo->lconv->currency_symbol
                || !locinfo->lconv->mon_decimal_point || !locinfo->lconv->mon_thousands_sep
                || !locinfo->lconv->mon_grouping || !locinfo->lconv->positive_sign
                || !locinfo->lconv->negative_sign) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lconv->int_curr_symbol[0] = '\0';
        locinfo->lconv->currency_symbol[0] = '\0';
        locinfo->lconv->mon_decimal_point[0] = '\0';
        locinfo->lconv->mon_thousands_sep[0] = '\0';
        locinfo->lconv->mon_grouping[0] = '\0';
        locinfo->lconv->positive_sign[0] = '\0';
        locinfo->lconv->negative_sign[0] = '\0';
        locinfo->lconv->int_frac_digits = charmax;
        locinfo->lconv->frac_digits = charmax;
        locinfo->lconv->p_cs_precedes = charmax;
        locinfo->lconv->p_sep_by_space = charmax;
        locinfo->lconv->n_cs_precedes = charmax;
        locinfo->lconv->n_sep_by_space = charmax;
        locinfo->lconv->p_sign_posn = charmax;
        locinfo->lconv->n_sign_posn = charmax;

#if _MSVCR_VER >= 100
        locinfo->lconv->_W_int_curr_symbol = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));
        locinfo->lconv->_W_currency_symbol = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));
        locinfo->lconv->_W_mon_decimal_point = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));
        locinfo->lconv->_W_mon_thousands_sep = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));
        locinfo->lconv->_W_positive_sign = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));
        locinfo->lconv->_W_negative_sign = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));

        if(!locinfo->lconv->_W_int_curr_symbol || !locinfo->lconv->_W_currency_symbol
                || !locinfo->lconv->_W_mon_decimal_point || !locinfo->lconv->_W_mon_thousands_sep
                || !locinfo->lconv->positive_sign || !locinfo->lconv->negative_sign) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lconv->_W_int_curr_symbol[0] = '\0';
        locinfo->lconv->_W_currency_symbol[0] = '\0';
        locinfo->lconv->_W_mon_decimal_point[0] = '\0';
        locinfo->lconv->_W_mon_thousands_sep[0] = '\0';
        locinfo->lconv->_W_positive_sign[0] = '\0';
        locinfo->lconv->_W_negative_sign[0] = '\0';
#endif

        locinfo->lc_category[MSVCRT_LC_MONETARY].locale = MSVCRT__strdup("C");
    }

    if(!category_needs_update(MSVCRT_LC_NUMERIC, category, old_locinfo,
                lcid[MSVCRT_LC_NUMERIC], cp[MSVCRT_LC_NUMERIC])) {
        locinfo->lc_handle[MSVCRT_LC_NUMERIC] = old_locinfo->lc_handle[MSVCRT_LC_NUMERIC];
        locinfo->lc_id[MSVCRT_LC_NUMERIC].wCodePage = old_locinfo->lc_id[MSVCRT_LC_NUMERIC].wCodePage;
    } else if(lcid[MSVCRT_LC_NUMERIC] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_NUMERIC)) {
        if(update_threadlocinfo_category(lcid[MSVCRT_LC_NUMERIC],
                    cp[MSVCRT_LC_NUMERIC], locinfo, MSVCRT_LC_NUMERIC)) {
            free_locinfo(locinfo);
            return NULL;
        }

        if(!locinfo->lconv_intl_refcount)
            locinfo->lconv_intl_refcount = MSVCRT_malloc(sizeof(int));
        locinfo->lconv_num_refcount = MSVCRT_malloc(sizeof(int));
        if(!locinfo->lconv_intl_refcount || !locinfo->lconv_num_refcount) {
            free_locinfo(locinfo);
            return NULL;
        }

        *locinfo->lconv_intl_refcount = 1;
        *locinfo->lconv_num_refcount = 1;

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_SDECIMAL
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->decimal_point = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->decimal_point, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_STHOUSAND
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i && (locinfo->lconv->thousands_sep = MSVCRT_malloc(i)))
            memcpy(locinfo->lconv->thousands_sep, buf, i);
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoA(lcid[MSVCRT_LC_NUMERIC], LOCALE_SGROUPING
                |LOCALE_NOUSEROVERRIDE, buf, 256);
        if(i>1)
            i = i/2 + (buf[i-2]=='0'?0:1);
        if(i && (locinfo->lconv->grouping = MSVCRT_malloc(i))) {
            for(i=0; buf[i+1]==';'; i+=2)
                locinfo->lconv->grouping[i/2] = buf[i]-'0';
            locinfo->lconv->grouping[i/2] = buf[i]-'0';
            if(buf[i] != '0')
                locinfo->lconv->grouping[i/2+1] = 127;
        } else {
            free_locinfo(locinfo);
            return NULL;
        }

#if _MSVCR_VER >= 100
        i = GetLocaleInfoW(lcid[MSVCRT_LC_NUMERIC], LOCALE_SDECIMAL
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_decimal_point = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_decimal_point, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }

        i = GetLocaleInfoW(lcid[MSVCRT_LC_NUMERIC], LOCALE_STHOUSAND
                |LOCALE_NOUSEROVERRIDE, wbuf, 256);
        if(i && (locinfo->lconv->_W_thousands_sep = MSVCRT_malloc(i * sizeof(MSVCRT_wchar_t))))
            memcpy(locinfo->lconv->_W_thousands_sep, wbuf, i * sizeof(MSVCRT_wchar_t));
        else {
            free_locinfo(locinfo);
            return NULL;
        }
#endif

        if(!set_lc_locale_name(locinfo, MSVCRT_LC_NUMERIC)) {
            free_locinfo(locinfo);
            return NULL;
        }
    } else {
        locinfo->lconv->decimal_point = MSVCRT_malloc(sizeof(char[2]));
        locinfo->lconv->thousands_sep = MSVCRT_malloc(sizeof(char));
        locinfo->lconv->grouping = MSVCRT_malloc(sizeof(char));
        if(!locinfo->lconv->decimal_point || !locinfo->lconv->thousands_sep
                || !locinfo->lconv->grouping) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lconv->decimal_point[0] = '.';
        locinfo->lconv->decimal_point[1] = '\0';
        locinfo->lconv->thousands_sep[0] = '\0';
        locinfo->lconv->grouping[0] = '\0';

#if _MSVCR_VER >= 100
        locinfo->lconv->_W_decimal_point = MSVCRT_malloc(sizeof(MSVCRT_wchar_t[2]));
        locinfo->lconv->_W_thousands_sep = MSVCRT_malloc(sizeof(MSVCRT_wchar_t));

        if(!locinfo->lconv->_W_decimal_point || !locinfo->lconv->_W_thousands_sep) {
            free_locinfo(locinfo);
            return NULL;
        }

        locinfo->lconv->_W_decimal_point[0] = '.';
        locinfo->lconv->_W_decimal_point[1] = '\0';
        locinfo->lconv->_W_thousands_sep[0] = '\0';
#endif

        locinfo->lc_category[MSVCRT_LC_NUMERIC].locale = MSVCRT__strdup("C");
    }

    if(!category_needs_update(MSVCRT_LC_TIME, category, old_locinfo,
                lcid[MSVCRT_LC_TIME], cp[MSVCRT_LC_TIME])) {
        locinfo->lc_handle[MSVCRT_LC_TIME] = old_locinfo->lc_handle[MSVCRT_LC_TIME];
        locinfo->lc_id[MSVCRT_LC_TIME].wCodePage = old_locinfo->lc_id[MSVCRT_LC_TIME].wCodePage;
    } else {
        DWORD flags = lcid[MSVCRT_LC_TIME] ? 0 : LOCALE_NOUSEROVERRIDE;

        if(lcid[MSVCRT_LC_TIME] && (category==MSVCRT_LC_ALL || category==MSVCRT_LC_TIME)) {
            if(update_threadlocinfo_category(lcid[MSVCRT_LC_TIME],
                        cp[MSVCRT_LC_TIME], locinfo, MSVCRT_LC_TIME)) {
                free_locinfo(locinfo);
                return NULL;
            }

            if(!set_lc_locale_name(locinfo, MSVCRT_LC_TIME)) {
                free_locinfo(locinfo);
                return NULL;
            }
        } else
            locinfo->lc_category[MSVCRT_LC_TIME].locale = MSVCRT__strdup("C");

        size = sizeof(MSVCRT___lc_time_data);
        lcid_tmp = lcid[MSVCRT_LC_TIME] ? lcid[MSVCRT_LC_TIME] : MAKELCID(LANG_ENGLISH, SORT_DEFAULT);
        for(i=0; i<sizeof(time_data)/sizeof(time_data[0]); i++) {
            if(time_data[i]==LOCALE_SSHORTDATE && !lcid[MSVCRT_LC_TIME]) {
                size += sizeof(cloc_short_date) + sizeof(cloc_short_dateW);
            }else if(time_data[i]==LOCALE_SLONGDATE && !lcid[MSVCRT_LC_TIME]) {
                size += sizeof(cloc_long_date) + sizeof(cloc_long_dateW);
            }else {
                ret = GetLocaleInfoA(lcid_tmp, time_data[i]|flags, NULL, 0);
                if(!ret) {
                    free_locinfo(locinfo);
                    return NULL;
                }
                size += ret;

                ret = GetLocaleInfoW(lcid_tmp, time_data[i]|flags, NULL, 0);
                if(!ret) {
                    free_locinfo(locinfo);
                    return NULL;
                }
                size += ret*sizeof(MSVCRT_wchar_t);
            }
        }
#if _MSVCR_VER >= 110
        size += LCIDToLocaleName(lcid[MSVCRT_LC_TIME], NULL, 0, 0)*sizeof(MSVCRT_wchar_t);
#endif

        locinfo->lc_time_curr = MSVCRT_malloc(size);
        if(!locinfo->lc_time_curr) {
            free_locinfo(locinfo);
            return NULL;
        }

        ret = 0;
        for(i=0; i<sizeof(time_data)/sizeof(time_data[0]); i++) {
            locinfo->lc_time_curr->str.str[i] = &locinfo->lc_time_curr->data[ret];
            if(time_data[i]==LOCALE_SSHORTDATE && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_short_date, sizeof(cloc_short_date));
                ret += sizeof(cloc_short_date);
            }else if(time_data[i]==LOCALE_SLONGDATE && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_long_date, sizeof(cloc_long_date));
                ret += sizeof(cloc_long_date);
            }else if(time_data[i]==LOCALE_STIMEFORMAT && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_time, sizeof(cloc_time));
                ret += sizeof(cloc_time);
            }else {
                ret += GetLocaleInfoA(lcid_tmp, time_data[i]|flags,
                    &locinfo->lc_time_curr->data[ret], size-ret);
            }
        }
        for(i=0; i<sizeof(time_data)/sizeof(time_data[0]); i++) {
            locinfo->lc_time_curr->wstr.wstr[i] = (MSVCRT_wchar_t*)&locinfo->lc_time_curr->data[ret];
            if(time_data[i]==LOCALE_SSHORTDATE && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_short_dateW, sizeof(cloc_short_dateW));
                ret += sizeof(cloc_short_dateW);
            }else if(time_data[i]==LOCALE_SLONGDATE && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_long_dateW, sizeof(cloc_long_dateW));
                ret += sizeof(cloc_long_dateW);
            }else if(time_data[i]==LOCALE_STIMEFORMAT && !lcid[MSVCRT_LC_TIME]) {
                memcpy(&locinfo->lc_time_curr->data[ret], cloc_timeW, sizeof(cloc_timeW));
                ret += sizeof(cloc_timeW);
            }else {
                ret += GetLocaleInfoW(lcid_tmp, time_data[i]|flags,
                        (MSVCRT_wchar_t*)&locinfo->lc_time_curr->data[ret], size-ret)*sizeof(MSVCRT_wchar_t);
            }
        }
#if _MSVCR_VER >= 110
        locinfo->lc_time_curr->locname = (MSVCRT_wchar_t*)&locinfo->lc_time_curr->data[ret];
        LCIDToLocaleName(lcid[MSVCRT_LC_TIME], locinfo->lc_time_curr->locname,
                (size-ret)/sizeof(MSVCRT_wchar_t), 0);
#else
        locinfo->lc_time_curr->lcid = lcid[MSVCRT_LC_TIME];
#endif
    }

    return locinfo;
}

/*********************************************************************
 *      _lock_locales (UCRTBASE.@)
 */
void CDECL _lock_locales(void)
{
    _mlock(_SETLOCALE_LOCK);
}

/*********************************************************************
 *      _unlock_locales (UCRTBASE.@)
 */
void CDECL _unlock_locales(void)
{
    _munlock(_SETLOCALE_LOCK);
}

/*********************************************************************
 *      _create_locale (MSVCRT.@)
 */
MSVCRT__locale_t CDECL MSVCRT__create_locale(int category, const char *locale)
{
    MSVCRT__locale_t loc;

    loc = MSVCRT_malloc(sizeof(MSVCRT__locale_tstruct));
    if(!loc)
        return NULL;

    loc->locinfo = create_locinfo(category, locale, NULL);
    if(!loc->locinfo) {
        MSVCRT_free(loc);
        return NULL;
    }

    loc->mbcinfo = MSVCRT_malloc(sizeof(MSVCRT_threadmbcinfo));
    if(!loc->mbcinfo) {
        free_locinfo(loc->locinfo);
        MSVCRT_free(loc);
        return NULL;
    }

    loc->mbcinfo->refcount = 1;
    _setmbcp_l(loc->locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage,
            loc->locinfo->lc_handle[MSVCRT_LC_CTYPE], loc->mbcinfo);
    return loc;
}

#if _MSVCR_VER >= 110
/*********************************************************************
 *      _wcreate_locale (MSVCR110.@)
 */
MSVCRT__locale_t CDECL MSVCRT__wcreate_locale(int category, const MSVCRT_wchar_t *locale)
{
    MSVCRT__locale_t loc;
    MSVCRT_size_t len;
    char *str;

    if(category<MSVCRT_LC_MIN || category>MSVCRT_LC_MAX || !locale)
        return NULL;

    len = MSVCRT_wcstombs(NULL, locale, 0);
    if(len == -1)
        return NULL;
    if(!(str = MSVCRT_malloc(++len)))
        return NULL;
    MSVCRT_wcstombs(str, locale, len);

    loc = MSVCRT__create_locale(category, str);

    MSVCRT_free(str);
    return loc;
}
#endif

/*********************************************************************
 *             setlocale (MSVCRT.@)
 */
char* CDECL MSVCRT_setlocale(int category, const char* locale)
{
    MSVCRT_pthreadlocinfo locinfo = get_locinfo();
    MSVCRT_pthreadlocinfo newlocinfo;

    if(category<MSVCRT_LC_MIN || category>MSVCRT_LC_MAX)
        return NULL;

    if(!locale) {
        if(category == MSVCRT_LC_ALL)
            return construct_lc_all(locinfo);

        return locinfo->lc_category[category].locale;
    }

    newlocinfo = create_locinfo(category, locale, locinfo);
    if(!newlocinfo) {
        WARN("%d %s failed\n", category, locale);
        return NULL;
    }

    _lock_locales();

    if(locinfo->lc_handle[MSVCRT_LC_COLLATE]!=newlocinfo->lc_handle[MSVCRT_LC_COLLATE]
            || locinfo->lc_id[MSVCRT_LC_COLLATE].wCodePage!=newlocinfo->lc_id[MSVCRT_LC_COLLATE].wCodePage) {
        locinfo->lc_collate_cp = newlocinfo->lc_collate_cp;
        locinfo->lc_handle[MSVCRT_LC_COLLATE] =
            newlocinfo->lc_handle[MSVCRT_LC_COLLATE];
        locinfo->lc_id[MSVCRT_LC_COLLATE] =
            newlocinfo->lc_id[MSVCRT_LC_COLLATE];
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_COLLATE].locale,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_COLLATE].locale);
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_COLLATE].refcount,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_COLLATE].refcount);

#if _MSVCR_VER >= 110
        swap_pointers((void**)&locinfo->lc_name[MSVCRT_LC_COLLATE],
                (void**)&newlocinfo->lc_name[MSVCRT_LC_COLLATE]);
#endif
    }

    if(locinfo->lc_handle[MSVCRT_LC_CTYPE]!=newlocinfo->lc_handle[MSVCRT_LC_CTYPE]
            || locinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage!=newlocinfo->lc_id[MSVCRT_LC_CTYPE].wCodePage) {
        locinfo->lc_handle[MSVCRT_LC_CTYPE] =
            newlocinfo->lc_handle[MSVCRT_LC_CTYPE];
        locinfo->lc_id[MSVCRT_LC_CTYPE] =
            newlocinfo->lc_id[MSVCRT_LC_CTYPE];
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_CTYPE].locale,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_CTYPE].locale);
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_CTYPE].refcount,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_CTYPE].refcount);

        locinfo->lc_codepage = newlocinfo->lc_codepage;
        locinfo->lc_clike = newlocinfo->lc_clike;
        locinfo->mb_cur_max = newlocinfo->mb_cur_max;

        swap_pointers((void**)&locinfo->ctype1_refcount,
                (void**)&newlocinfo->ctype1_refcount);
        swap_pointers((void**)&locinfo->ctype1, (void**)&newlocinfo->ctype1);
        swap_pointers((void**)&locinfo->pctype, (void**)&newlocinfo->pctype);
        swap_pointers((void**)&locinfo->pclmap, (void**)&newlocinfo->pclmap);
        swap_pointers((void**)&locinfo->pcumap, (void**)&newlocinfo->pcumap);

#if _MSVCR_VER >= 110
        swap_pointers((void**)&locinfo->lc_name[MSVCRT_LC_CTYPE],
                (void**)&newlocinfo->lc_name[MSVCRT_LC_CTYPE]);
#endif
    }

    if(locinfo->lc_handle[MSVCRT_LC_MONETARY]!=newlocinfo->lc_handle[MSVCRT_LC_MONETARY]
            || locinfo->lc_id[MSVCRT_LC_MONETARY].wCodePage!=newlocinfo->lc_id[MSVCRT_LC_MONETARY].wCodePage) {
        locinfo->lc_handle[MSVCRT_LC_MONETARY] =
            newlocinfo->lc_handle[MSVCRT_LC_MONETARY];
        locinfo->lc_id[MSVCRT_LC_MONETARY] =
            newlocinfo->lc_id[MSVCRT_LC_MONETARY];
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_MONETARY].locale,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_MONETARY].locale);
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_MONETARY].refcount,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_MONETARY].refcount);

        swap_pointers((void**)&locinfo->lconv->int_curr_symbol,
                (void**)&newlocinfo->lconv->int_curr_symbol);
        swap_pointers((void**)&locinfo->lconv->currency_symbol,
                (void**)&newlocinfo->lconv->currency_symbol);
        swap_pointers((void**)&locinfo->lconv->mon_decimal_point,
                (void**)&newlocinfo->lconv->mon_decimal_point);
        swap_pointers((void**)&locinfo->lconv->mon_thousands_sep,
                (void**)&newlocinfo->lconv->mon_thousands_sep);
        swap_pointers((void**)&locinfo->lconv->mon_grouping,
                (void**)&newlocinfo->lconv->mon_grouping);
        swap_pointers((void**)&locinfo->lconv->positive_sign,
                (void**)&newlocinfo->lconv->positive_sign);
        swap_pointers((void**)&locinfo->lconv->negative_sign,
                (void**)&newlocinfo->lconv->negative_sign);

#if _MSVCR_VER >= 100
        swap_pointers((void**)&locinfo->lconv->_W_int_curr_symbol,
                (void**)&newlocinfo->lconv->_W_int_curr_symbol);
        swap_pointers((void**)&locinfo->lconv->_W_currency_symbol,
                (void**)&newlocinfo->lconv->_W_currency_symbol);
        swap_pointers((void**)&locinfo->lconv->_W_mon_decimal_point,
                (void**)&newlocinfo->lconv->_W_mon_decimal_point);
        swap_pointers((void**)&locinfo->lconv->_W_mon_thousands_sep,
                (void**)&newlocinfo->lconv->_W_mon_thousands_sep);
        swap_pointers((void**)&locinfo->lconv->_W_positive_sign,
                (void**)&newlocinfo->lconv->_W_positive_sign);
        swap_pointers((void**)&locinfo->lconv->_W_negative_sign,
                (void**)&newlocinfo->lconv->_W_negative_sign);
#endif

        locinfo->lconv->int_frac_digits = newlocinfo->lconv->int_frac_digits;
        locinfo->lconv->frac_digits = newlocinfo->lconv->frac_digits;
        locinfo->lconv->p_cs_precedes = newlocinfo->lconv->p_cs_precedes;
        locinfo->lconv->p_sep_by_space = newlocinfo->lconv->p_sep_by_space;
        locinfo->lconv->n_cs_precedes = newlocinfo->lconv->n_cs_precedes;
        locinfo->lconv->n_sep_by_space = newlocinfo->lconv->n_sep_by_space;
        locinfo->lconv->p_sign_posn = newlocinfo->lconv->p_sign_posn;
        locinfo->lconv->n_sign_posn = newlocinfo->lconv->n_sign_posn;

#if _MSVCR_VER >= 110
        swap_pointers((void**)&locinfo->lc_name[MSVCRT_LC_MONETARY],
                (void**)&newlocinfo->lc_name[MSVCRT_LC_MONETARY]);
#endif
    }

    if(locinfo->lc_handle[MSVCRT_LC_NUMERIC]!=newlocinfo->lc_handle[MSVCRT_LC_NUMERIC]
            || locinfo->lc_id[MSVCRT_LC_NUMERIC].wCodePage!=newlocinfo->lc_id[MSVCRT_LC_NUMERIC].wCodePage) {
        locinfo->lc_handle[MSVCRT_LC_NUMERIC] =
            newlocinfo->lc_handle[MSVCRT_LC_NUMERIC];
        locinfo->lc_id[MSVCRT_LC_NUMERIC] =
            newlocinfo->lc_id[MSVCRT_LC_NUMERIC];
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_NUMERIC].locale,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_NUMERIC].locale);
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_NUMERIC].refcount,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_NUMERIC].refcount);

        swap_pointers((void**)&locinfo->lconv->decimal_point,
                (void**)&newlocinfo->lconv->decimal_point);
        swap_pointers((void**)&locinfo->lconv->thousands_sep,
                (void**)&newlocinfo->lconv->thousands_sep);
        swap_pointers((void**)&locinfo->lconv->grouping,
                (void**)&newlocinfo->lconv->grouping);

#if _MSVCR_VER >= 100
        swap_pointers((void**)&locinfo->lconv->_W_decimal_point,
                (void**)&newlocinfo->lconv->_W_decimal_point);
        swap_pointers((void**)&locinfo->lconv->_W_thousands_sep,
                (void**)&newlocinfo->lconv->_W_thousands_sep);
#endif

#if _MSVCR_VER >= 110
        swap_pointers((void**)&locinfo->lc_name[MSVCRT_LC_NUMERIC],
                (void**)&newlocinfo->lc_name[MSVCRT_LC_NUMERIC]);
#endif
    }

    if(locinfo->lc_handle[MSVCRT_LC_TIME]!=newlocinfo->lc_handle[MSVCRT_LC_TIME]
            || locinfo->lc_id[MSVCRT_LC_TIME].wCodePage!=newlocinfo->lc_id[MSVCRT_LC_TIME].wCodePage) {
        locinfo->lc_handle[MSVCRT_LC_TIME] =
            newlocinfo->lc_handle[MSVCRT_LC_TIME];
        locinfo->lc_id[MSVCRT_LC_TIME] =
            newlocinfo->lc_id[MSVCRT_LC_TIME];
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_TIME].locale,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_TIME].locale);
        swap_pointers((void**)&locinfo->lc_category[MSVCRT_LC_TIME].refcount,
                (void**)&newlocinfo->lc_category[MSVCRT_LC_TIME].refcount);
        swap_pointers((void**)&locinfo->lc_time_curr,
                (void**)&newlocinfo->lc_time_curr);

#if _MSVCR_VER >= 110
        swap_pointers((void**)&locinfo->lc_name[MSVCRT_LC_TIME],
                (void**)&newlocinfo->lc_name[MSVCRT_LC_TIME]);
#endif
    }

    free_locinfo(newlocinfo);
    _unlock_locales();

    if(locinfo == MSVCRT_locale->locinfo) {
        int i;

        MSVCRT___lc_codepage = locinfo->lc_codepage;
        MSVCRT___lc_collate_cp = locinfo->lc_collate_cp;
        MSVCRT___mb_cur_max = locinfo->mb_cur_max;
        MSVCRT__pctype = locinfo->pctype;
        for(i=MSVCRT_LC_MIN; i<=MSVCRT_LC_MAX; i++)
            MSVCRT___lc_handle[i] = MSVCRT_locale->locinfo->lc_handle[i];
    }

    if(category == MSVCRT_LC_ALL)
        return construct_lc_all(locinfo);

    return locinfo->lc_category[category].locale;
}

/*********************************************************************
 *		_wsetlocale (MSVCRT.@)
 */
MSVCRT_wchar_t* CDECL MSVCRT__wsetlocale(int category, const MSVCRT_wchar_t* wlocale)
{
    static MSVCRT_wchar_t current_lc_all[MAX_LOCALE_LENGTH];

    char *locale = NULL;
    const char *ret;
    MSVCRT_size_t len;

    if(wlocale) {
        len = MSVCRT_wcstombs(NULL, wlocale, 0);
        if(len == -1)
            return NULL;

        locale = MSVCRT_malloc(++len);
        if(!locale)
            return NULL;

        MSVCRT_wcstombs(locale, wlocale, len);
    }

    _lock_locales();
    ret = MSVCRT_setlocale(category, locale);
    MSVCRT_free(locale);

    if(ret && MSVCRT_mbstowcs(current_lc_all, ret, MAX_LOCALE_LENGTH)==-1)
        ret = NULL;

    _unlock_locales();
    return ret ? current_lc_all : NULL;
}

#if _MSVCR_VER >= 80
/*********************************************************************
 *		_configthreadlocale (MSVCR80.@)
 */
int CDECL _configthreadlocale(int type)
{
    thread_data_t *data = msvcrt_get_thread_data();
    MSVCRT__locale_t locale;
    int ret;

    if(!data)
        return -1;

    ret = (data->have_locale ? MSVCRT__ENABLE_PER_THREAD_LOCALE : MSVCRT__DISABLE_PER_THREAD_LOCALE);

    if(type == MSVCRT__ENABLE_PER_THREAD_LOCALE) {
        if(!data->have_locale) {
            /* Copy current global locale */
            locale = MSVCRT__create_locale(MSVCRT_LC_ALL, MSVCRT_setlocale(MSVCRT_LC_ALL, NULL));
            if(!locale)
                return -1;

            data->locinfo = locale->locinfo;
            data->mbcinfo = locale->mbcinfo;
            data->have_locale = TRUE;
            MSVCRT_free(locale);
        }

        return ret;
    }

    if(type == MSVCRT__DISABLE_PER_THREAD_LOCALE) {
        if(data->have_locale) {
            free_locinfo(data->locinfo);
            free_mbcinfo(data->mbcinfo);
            data->locinfo = MSVCRT_locale->locinfo;
            data->mbcinfo = MSVCRT_locale->mbcinfo;
            data->have_locale = FALSE;
        }

        return ret;
    }

    if(!type)
        return ret;

    return -1;
}
#endif

BOOL msvcrt_init_locale(void)
{
    int i;

    _lock_locales();
    MSVCRT_locale = MSVCRT__create_locale(0, "C");
    _unlock_locales();
    if(!MSVCRT_locale)
        return FALSE;

    MSVCRT___lc_codepage = MSVCRT_locale->locinfo->lc_codepage;
    MSVCRT___lc_collate_cp = MSVCRT_locale->locinfo->lc_collate_cp;
    MSVCRT___mb_cur_max = MSVCRT_locale->locinfo->mb_cur_max;
    MSVCRT__pctype = MSVCRT_locale->locinfo->pctype;
    for(i=MSVCRT_LC_MIN; i<=MSVCRT_LC_MAX; i++)
        MSVCRT___lc_handle[i] = MSVCRT_locale->locinfo->lc_handle[i];
    _setmbcp(_MB_CP_ANSI);
    return TRUE;
}
