// GTA IV PS5 - languages: the hub's text in English, French, German, Spanish
// and Italian (the five the Xbox 360 disc has the game's text in), and the
// Language setting (ps5_language) that picks one for the hub and the game.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace gta4ps5::hub::lang {

enum Lang { kEnglish, kFrench, kGerman, kSpanish, kItalian, kLangCount };

// The PS5's own language, as one of the five (English when it's another).
Lang System();
// The setting's value ("auto", "english", "french"...): auto is the PS5's.
Lang FromSetting(const std::string& value);
// What the game is told (the Xbox 360's XCONFIG user language): the game
// reads its text from the matching .gxt in common.rpf.
uint32_t XboxLanguage(Lang l);
// The language's name, in itself ("Français").
const char* Name(Lang l);

void Set(Lang l);
Lang Current();

// `english` in the hub's language (itself when there's no translation).
// A key may carry a note after '|' ("Earned|date") when one English word
// needs two translations; the note isn't shown.
const char* T(const char* english);
// Whether `english` has translations (tools/hub_preview checks the hub's text).
bool Has(const char* english);
// T, then each "{}" replaced by the next argument.
std::string F(const char* english, std::initializer_list<std::string> args);
// A size or rate as the language writes it ("1.5 GB" -> "1,5 Go").
std::string Units(std::string s);
// A date and time (unix time; "-" for none).
std::string Date(int64_t t);
// The footer's clock, as the game's pause menu has its day and time: the
// day of the week, the date and the time on a 12-hour clock ("WED / OCT 21 /
// 8:45 PM"), in capitals.
std::string Clock(int64_t t);

}  // namespace gta4ps5::hub::lang
