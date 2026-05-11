if (NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED ARCH)
    message(FATAL_ERROR "patch_skel.cmake requires INPUT, OUTPUT, and ARCH")
endif()

file(READ "${INPUT}" SKEL_SOURCE)
string(REPLACE
    "libcdsp_peak_skel.so"
    "libcdsp_peak_skel_${ARCH}.so"
    SKEL_SOURCE
    "${SKEL_SOURCE}"
)
string(REGEX REPLACE
    "cdsp_peak_skel_handle_invoke_uri\\[[0-9]+\\+1\\]"
    "cdsp_peak_skel_handle_invoke_uri[]"
    SKEL_SOURCE
    "${SKEL_SOURCE}"
)
file(WRITE "${OUTPUT}" "${SKEL_SOURCE}")
