#pragma once
// thprac_gui_locale.h - Adapter for th06 source build
// Provides the locale system interface matching thprac's Gui namespace

#include "thprac_locale_def.h"

namespace THPrac {
namespace Gui {
    enum locale_t {
        LOCALE_NONE = -1,
        LOCALE_ZH_CN = 0,
        LOCALE_EN_US = 1,
        LOCALE_JA_JP = 2,
    };

    void LocaleSet(locale_t locale);
    void LocaleAutoSet();

    extern locale_t __glocale_current;

    inline locale_t LocaleGet()
    {
        return __glocale_current;
    }

    inline const char** LocaleGetCurrentGlossary()
    {
        return th_glossary_str[LocaleGet()];
    }

    inline const char* LocaleGetStr(th_glossary_t name)
    {
        return LocaleGetCurrentGlossary()[name];
    }

    void LocaleRotate();

// Section lookup macros (game-namespace dependent)
#define XCBA(stage, type) th_sections_cba[stage][type]
#define XSSS(rank) (th_sections_str[::THPrac::Gui::LocaleGet()][rank])

}

inline const char* S(th_glossary_t name)
{
    return Gui::LocaleGetStr(name);
}

}
