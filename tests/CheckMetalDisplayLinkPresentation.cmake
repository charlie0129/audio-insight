# SPDX-License-Identifier: AGPL-3.0-or-later

if(NOT DEFINED SOURCE_FILE OR NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "Metal visualization source was not found: ${SOURCE_FILE}")
endif()

file(READ "${SOURCE_FILE}" SOURCE_CONTENT)

set(
    FORBIDDEN_PRESENTATION_PATTERNS
    "presentDrawable[ \t]*:[^\r\n]*atTime[ \t]*:"
    "presentDrawable[ \t]*:[^\r\n]*afterMinimumDuration[ \t]*:"
    "presentAtTime[ \t]*:"
    "presentAfterMinimumDuration[ \t]*:"
)

foreach(FORBIDDEN_PATTERN IN LISTS FORBIDDEN_PRESENTATION_PATTERNS)
    if(SOURCE_CONTENT MATCHES "${FORBIDDEN_PATTERN}")
        message(
            FATAL_ERROR
            "CAMetalDisplayLink owns presentation timing; use plain MTLDrawable present()"
        )
    endif()
endforeach()
