# Main component for [env:xiao-w5500-idf].
#
# The Arduino environments exclude src/idf/ via build_src_filter, so both
# frameworks can share one PlatformIO project without seeing each other's
# sources. The HTML pages are embedded from the shared data/ directory through
# symlinks so the Arduino and IDF builds serve byte-identical UI files.
idf_component_register(
    SRCS
        "idf/atem.cpp"
        "idf/config.c"
        "idf/main.c"
        "idf/eth.c"
        "idf/espnow.c"
        "idf/mdns_adv.c"
        "idf/obs_client.c"
        "idf/osc.c"
        "idf/vmix_client.c"
        "idf/vmix_server.c"
        "idf/watchdog.c"
        "idf/web.c"
        "idf/ws.c"
    INCLUDE_DIRS "idf"
    PRIV_INCLUDE_DIRS "../lib/ATEMmini"
    REQUIRES
        cjson
        esp_driver_gpio
        esp_driver_spi
        esp_eth
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_websocket_client
        esp_wifi
        lwip
        mdns
        nvs_flash
)

# Mixing C and C++ here makes PlatformIO hand both languages the merged flag
# set; see build_unflags in [env:xiao-w5500-idf] for the fallout. The ATEM
# client gets its wire-format helpers from ../lib/ATEMmini, which PlatformIO
# builds and links as a separate library.

# ESP-IDF's EMBED_FILES generates its .S file under the BUILD_DIR property,
# which PlatformIO passes as a *relative* path. The custom command then runs
# with that relative path as its working directory, the file lands in a doubled
# path (build/build/foo.S), and the build fails with "Source ... not found".
#
# Generate the same _binary_<name>_start/_end arrays ourselves at configure
# time using absolute paths. Output is 16 bytes per .byte line to stay well
# inside assembler line-length limits.
#
# PlatformIO does not re-run the configure step when data/*.html changes, so
# scripts/embed_pages.py rewrites these files before each build. Keep its PAGES
# list in sync with the embed_binary() calls below.
function(embed_binary target file symbol)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n" bytes "${bytes}")
    string(REGEX REPLACE ",$" "" bytes "${bytes}")

    set(asm "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.S")
    file(WRITE "${asm}"
        "/* Generated from ${file} by embed_binary(); do not edit. */\n"
        ".section .rodata.embedded\n"
        ".global ${symbol}_start\n"
        ".global ${symbol}_end\n"
        ".balign 4\n"
        "${symbol}_start:\n"
        ".byte ${bytes}\n"
        "${symbol}_end:\n")
    target_sources(${target} PRIVATE "${asm}")
endfunction()

embed_binary(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/idf/web/index.html" _binary_index_html)
embed_binary(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/idf/web/tally.html" _binary_tally_html)
embed_binary(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/idf/web/director.html" _binary_director_html)
