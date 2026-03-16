#include "thprac_gui_locale.h"

namespace THPrac {
namespace Gui {

locale_t __glocale_current = LOCALE_ZH_CN;

void LocaleSet(locale_t locale)
{
    if (locale >= LOCALE_ZH_CN && locale <= LOCALE_JA_JP)
        __glocale_current = locale;
}

void LocaleAutoSet()
{
    // Default to Chinese for now
    __glocale_current = LOCALE_ZH_CN;
}

void LocaleRotate()
{
    __glocale_current = (locale_t)((__glocale_current + 1) % 3);
}

}
}
