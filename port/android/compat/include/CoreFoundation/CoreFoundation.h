/* The CoreFoundation locale calls MacNativeGames makes (current language, script and region, and the preferred
   languages), answered from Android's persist.sys.locale property (a BCP-47 tag such as "en-US" or "zh-Hant-TW");
   TOTK_LOCALE overrides it in services.c. */
#pragma once
#include <ctype.h>
#include <string.h>
#include <sys/system_properties.h>
typedef const struct CompatLocale *CFLocaleRef;
typedef const char *CFStringRef;
typedef int CFStringEncoding;
typedef long CFIndex;
#define kCFStringEncodingUTF8 0x08000100
#define kCFLocaleLanguageCode ((CFStringRef)"language")
#define kCFLocaleCountryCode ((CFStringRef)"country")
#define kCFLocaleScriptCode ((CFStringRef)"script")
struct CompatLocale { char tag[PROP_VALUE_MAX], language[16], script[8], country[8]; };
static inline CFLocaleRef CFLocaleCopyCurrent(void) {
    static struct CompatLocale l; char tag[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.sys.locale", tag) <= 0) __system_property_get("ro.product.locale", tag);
    if (!*tag) strcpy(tag, "en-US");
    memcpy(l.tag, tag, sizeof l.tag);
    size_t n = strcspn(tag, "-_"); if (n >= sizeof l.language) n = sizeof l.language - 1;
    memcpy(l.language, tag, n); l.language[n] = 0;
    for (const char *p = tag + n; *p;) {
        p += strspn(p, "-_"); size_t k = strcspn(p, "-_");
        if (k == 2 && isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1])) { l.country[0] = (char)toupper((unsigned char)p[0]); l.country[1] = (char)toupper((unsigned char)p[1]); l.country[2] = 0; }
        if (k == 4 && !*l.script && isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[3]))
            for (size_t i = 0; i < 4; i++) { l.script[i] = (char)(i ? tolower((unsigned char)p[i]) : toupper((unsigned char)p[i])); l.script[4] = 0; }
        p += k;
    }
    return &l;
}
static inline CFStringRef CFLocaleGetValue(CFLocaleRef l, CFStringRef key) {
    const char *v = !strcmp(key, "language") ? l->language : !strcmp(key, "script") ? l->script : l->country;
    return *v ? v : NULL;
}
/* The preferred languages: the system locale's tag alone */
typedef const struct CompatLocale *CFArrayRef;
static inline CFArrayRef CFLocaleCopyPreferredLanguages(void) { return CFLocaleCopyCurrent(); }
static inline CFIndex CFArrayGetCount(CFArrayRef a) { return a ? 1 : 0; }
static inline const void *CFArrayGetValueAtIndex(CFArrayRef a, CFIndex i) { (void)i; return a->tag; }
static inline int CFStringGetCString(CFStringRef s, char *buffer, CFIndex size, CFStringEncoding e) {
    (void)e; if (!s || size <= 0) return 0; strncpy(buffer, s, (size_t)size - 1); buffer[size - 1] = 0; return 1;
}
static inline void CFRelease(const void *p) { (void)p; }
