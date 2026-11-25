# Writes the art's pictures (every PNG under ROOT) into OUT as a C table, zb_art_files, which the app reads
# them from (src/ui/art/art_view.cpp). Pure CMake, run by the build whenever a picture changes:
#     cmake -DROOT=assets/art -DOUT=art_files.c -P cmake/embed.cmake
file(GLOB_RECURSE pictures RELATIVE "${ROOT}" "${ROOT}/*.png")
list(SORT pictures)
set(body "/* Written by cmake/embed.cmake from assets/art: do not edit. */\n")
set(table "")
set(count 0)
foreach(picture IN LISTS pictures)
    file(READ "${ROOT}/${picture}" bytes HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${bytes}")
    string(APPEND body "static const unsigned char file${count}[] = {${bytes}};\n")
    string(APPEND table "    {\"${picture}\", file${count}, sizeof file${count}},\n")
    math(EXPR count "${count} + 1")
endforeach()
string(APPEND body "struct ZbArtFile { const char *path; const unsigned char *data; unsigned long size; };\n"
                   "const struct ZbArtFile zb_art_files[] = {\n${table}    {0, 0, 0}\n};\n"
                   "const unsigned long zb_art_file_count = ${count};\n")
file(WRITE "${OUT}" "${body}")
