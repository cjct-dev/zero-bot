// Card and ID numbers, which never leave the device in a search query (to a search service) or a memory (to the AI
// service, with every request). One check, made by web_search before a search is sent and by memory_save before a
// memory is saved. Definitions in sensitive.cpp.
#pragma once
#include <string>

namespace tools {

// What kind of number `text` holds, as words for a sentence ("a payment card number"), or "" when it holds none.
// Only the real formats count, so that a phone number, an order number, an IMEI, a run of years, a date, an ISBN, a
// price or a postcode never does:
//   a payment card number: one a card network issues, by its prefix and length (Visa 4: 13, 16 or 19 digits;
//     Mastercard 51-55 and 2221-2720: 16; American Express 34 and 37: 15; Discover 6011, 644-649 and 65, JCB
//     3528-3589, UnionPay 62 and Mir 2200-2204: 16 to 19; Diners Club 300-305, 36, 38 and 39: 14 to 19; Maestro 50,
//     56-58 and 6759: 12 to 19; RuPay 60, 65, 81, 82 and 508: 16; Verve 5061 and 650002-650027: 16 to 19), passing
//     the Luhn check, run together or in the groups cards are written in (fours, or 4-6-5 and 4-6-4), and no "+" in
//     front, where a phone number has one, full-width or not;
//   an IBAN: a country's code, two check digits and up to 30 letters and digits, run together or in fours (the code
//     and the check digits may stand apart: "GB 82 WEST ..."), passing the mod-97 check;
//   a UK National Insurance number: two letters of the allowed prefixes, six digits and A to D, as written
//     with or without spaces ("QQ 12 34 56 C" is HMRC's example, and QQ is never issued);
//   a US Social Security number: ddd-dd-dddd, but no area 000, 666 or 900 to 999, no group 00, no serial 0000.
// Parts may stand apart by one space, dash, full stop or slash, or " - ", the same throughout. Digits of any script
// (full-width, Arabic-Indic, ...) and full-width letters count as their ASCII ones; Unicode's other spaces and dashes
// as those; characters that show as nothing, and the joiners and styles clean_text keeps, as nothing.
std::string sensitive_number(const std::string &text);

}  // namespace tools
