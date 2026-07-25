# ttl2dvi_core: the shared module library that apps link. Grows as modules are
# added (capture, video, ...). For now: the console, plus config headers
# (board.h, version.h) exposed to consumers.
add_library(ttl2dvi_core STATIC
    console/console.c
)

# PUBLIC so apps that link ttl2dvi_core also see these headers + pico_stdlib.
target_include_directories(ttl2dvi_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/console
    ${CMAKE_CURRENT_LIST_DIR}/config
)
target_link_libraries(ttl2dvi_core PUBLIC
    pico_stdlib
)
