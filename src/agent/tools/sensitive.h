// Card and ID numbers, which never leave the device in a search query (to a search service) or a memory (to the AI
// service, with every request). One check, made by web_search before a search is sent and by memory_save before a
// memory is saved. Definitions in sensitive.cpp.
#pragma once
#include <string>

namespace tools {

// What kind of number `text` holds, as words for a sentence ("a payment card number"), or "" when it holds none.
// Only the real formats count, so that a phone number, an order number, a date, an ISBN, a price or a postcode
// never does:
//   a payment card number: 13 to 19 digits starting with 2 to 6 (the card networks' ranges), passing the Luhn
//     check, run together or in the groups cards are written in (fours, or 4-6-5 and 4-6-4), with single spaces
//     or dashes between them, and no "+" in front (a phone number's);
//   an IBAN: a country's code, two check digits and up to 30 letters and digits, run together or in fours,
//     passing the mod-97 check;
//   a UK National Insurance number: two letters of the allowed prefixes, six digits and A to D, as written
//     with or without spaces ("QQ 12 34 56 C" is HMRC's example, and QQ is never issued);
//   a US Social Security number: ddd-dd-dddd, but no area 000, 666 or 900 to 999, no group 00, no serial 0000.
// Characters that show as nothing are seen through (clean_text), as are Unicode's other spaces and dashes.
std::string sensitive_number(const std::string &text);

}  // namespace tools
