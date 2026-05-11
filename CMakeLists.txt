cmake_minimum_required(VERSION 3.24)

project(cdsp_peak C)

set(CDSP_PEAK_SDK_ROOT "$ENV{HEXAGON_SDK_ROOT}" CACHE PATH "Hexagon SDK root")
if (NOT CDSP_PEAK_SDK_ROOT)
    set(CDSP_PEAK_SDK_ROOT "C:/Qualcomm/Hexagon_SDK/6.5.0.1")
endif()
file(TO_CMAKE_PATH "${CDSP_PEAK_SDK_ROOT}" CDSP_PEAK_SDK_ROOT)

if (NOT IS_DIRECTORY "${CDSP_PEAK_SDK_ROOT}")
    message(FATAL_ERROR "CDSP_PEAK_SDK_ROOT does not point to a Hexagon SDK: ${CDSP_PEAK_SDK_ROOT}")
endif()

set(HEXAGON_TOOLS_ROOT "$ENV{HEXAGON_TOOLS_ROOT}" CACHE PATH "Hexagon tools root")
if (NOT HEXAGON_TOOLS_ROOT)
    file(READ "${CDSP_PEAK_SDK_ROOT}/hexagon_sdk.json" CDSP_PEAK_SDK_JSON)
    string(JSON HEXAGON_TOOLS_PATH GET "${CDSP_PEAK_SDK_JSON}" root tools info 0 path)
    set(HEXAGON_TOOLS_ROOT "${CDSP_PEAK_SDK_ROOT}/${HEXAGON_TOOLS_PATH}")
endif()
file(TO_CMAKE_PATH "${HEXAGON_TOOLS_ROOT}" HEXAGON_TOOLS_ROOT)

if (WIN32)
    set(HOST_QAIC "${CDSP_PEAK_SDK_ROOT}/ipc/fastrpc/qaic/WinNT/qaic.exe")
    set(HEXAGON_TOOL_SUFFIX ".exe")
else()
    set(HOST_QAIC "${CDSP_PEAK_SDK_ROOT}/ipc/fastrpc/qaic/Ubuntu/qaic")
    set(HEXAGON_TOOL_SUFFIX "")
endif()

set(HEXAGON_CC "${HEXAGON_TOOLS_ROOT}/Tools/bin/hexagon-clang${HEXAGON_TOOL_SUFFIX}" CACHE FILEPATH "Hexagon C compiler")

if (NOT EXISTS "${HOST_QAIC}")
    message(FATAL_ERROR "qaic not found: ${HOST_QAIC}")
endif()
if (NOT EXISTS "${HEXAGON_CC}")
    message(FATAL_ERROR "hexagon-clang not found: ${HEXAGON_CC}")
endif()

set(CDSP_PEAK_DSP_ARCHES v68 v69 v73 v75 v79 v81 CACHE STRING "Hexagon skel architectures")
set(CDSP_PEAK_HTP_CERT "$ENV{HEXAGON_HTP_CERT}" CACHE FILEPATH "Optional PFX certificate for signing Windows HTP skel catalog")
set(CDSP_PEAK_HTP_CERT_PASSWORD "$ENV{HEXAGON_HTP_CERT_PASSWORD}" CACHE STRING "Optional password for CDSP_PEAK_HTP_CERT")

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(GEN_HEADER "${GEN_DIR}/cdsp_peak.h")
set(GEN_STUB_C "${GEN_DIR}/cdsp_peak_stub.c")
set(GEN_SKEL_C "${GEN_DIR}/cdsp_peak_skel.c")

add_custom_command(
    OUTPUT "${GEN_HEADER}" "${GEN_STUB_C}" "${GEN_SKEL_C}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN_DIR}"
    COMMAND "${HOST_QAIC}"
        -mdll
        -I "${CDSP_PEAK_SDK_ROOT}/incs"
        -I "${CDSP_PEAK_SDK_ROOT}/incs/stddef"
        -I "${CDSP_PEAK_SDK_ROOT}/ipc/fastrpc/incs"
        -o "${GEN_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/idl/cdsp_peak.idl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/idl/cdsp_peak.idl"
    VERBATIM
)

add_custom_target(cdsp_peak_qaic DEPENDS "${GEN_HEADER}" "${GEN_STUB_C}" "${GEN_SKEL_C}")

set(SDK_INCLUDE_DIRS
    "${CDSP_PEAK_SDK_ROOT}/incs"
    "${CDSP_PEAK_SDK_ROOT}/incs/stddef"
    "${CDSP_PEAK_SDK_ROOT}/ipc/fastrpc/incs"
    "${CDSP_PEAK_SDK_ROOT}/ipc/fastrpc/rpcmem/inc"
)

add_executable(cdsp_peak
    host/cdsp_peak.c
    "${GEN_STUB_C}"
)
add_dependencies(cdsp_peak cdsp_peak_qaic)

target_include_directories(cdsp_peak PRIVATE
    "${GEN_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/host"
    ${SDK_INCLUDE_DIRS}
)
target_compile_definitions(cdsp_peak PRIVATE STATIC_LIB)

if (WIN32)
    target_sources(cdsp_peak PRIVATE
        host/fastrpc_windows.c
        host/windows_compat.c
    )
    target_compile_definitions(cdsp_peak PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(cdsp_peak PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/host/windows_compat.h"
    )
    target_link_libraries(cdsp_peak PRIVATE advapi32)
else()
    target_link_libraries(cdsp_peak PRIVATE pthread dl cdsprpc bsd)
endif()

set(DSP_INCLUDE_DIRS
    "${GEN_DIR}"
    ${SDK_INCLUDE_DIRS}
    "${HEXAGON_TOOLS_ROOT}/Tools/target/hexagon/include"
)

set(DSP_SKELS)
foreach (ARCH IN LISTS CDSP_PEAK_DSP_ARCHES)
    string(SUBSTRING "${ARCH}" 1 -1 ARCH_NUM)
    set(DSP_DIR "${CMAKE_CURRENT_BINARY_DIR}/dsp/${ARCH}")
    set(PATCHED_SKEL_C "${DSP_DIR}/cdsp_peak_skel_${ARCH}.c")
    set(SKEL_OBJ "${DSP_DIR}/cdsp_peak_skel.o")
    set(IMP_OBJ "${DSP_DIR}/cdsp_peak_imp.o")
    set(POWER_OBJ "${DSP_DIR}/cdsp_peak_power.o")
    set(HMX_OBJ "${DSP_DIR}/cdsp_peak_hmx.o")
    set(SKEL_SO "${DSP_DIR}/libcdsp_peak_skel_${ARCH}.so")

    set(DSP_COMMON_FLAGS
        -O3 -G0 -fPIC -Wall -Werror
        -DCDSP_PEAK_ARCH=${ARCH_NUM}
        -m${ARCH}
        -mcpu=hexagon${ARCH}
        -mhvx=${ARCH}
        -mhvx-ieee-fp
    )
    foreach (INC_DIR IN LISTS DSP_INCLUDE_DIRS)
        list(APPEND DSP_COMMON_FLAGS "-I${INC_DIR}")
    endforeach()

    add_custom_command(
        OUTPUT "${PATCHED_SKEL_C}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DSP_DIR}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${GEN_SKEL_C}"
            "-DOUTPUT=${PATCHED_SKEL_C}"
            "-DARCH=${ARCH}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_skel.cmake"
        DEPENDS "${GEN_SKEL_C}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_skel.cmake"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${SKEL_OBJ}"
        COMMAND "${HEXAGON_CC}" ${DSP_COMMON_FLAGS} -c "${PATCHED_SKEL_C}" -o "${SKEL_OBJ}"
        DEPENDS "${PATCHED_SKEL_C}" "${GEN_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${IMP_OBJ}"
        COMMAND "${HEXAGON_CC}" ${DSP_COMMON_FLAGS} -c "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_imp.c" -o "${IMP_OBJ}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_imp.c" "${GEN_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${POWER_OBJ}"
        COMMAND "${HEXAGON_CC}" ${DSP_COMMON_FLAGS} -c "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_power.c" -o "${POWER_OBJ}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_power.c" "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_hap_power.h" "${GEN_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${HMX_OBJ}"
        COMMAND "${HEXAGON_CC}" ${DSP_COMMON_FLAGS} -mhmx -c "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_hmx.c" -o "${HMX_OBJ}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dsp/cdsp_peak_hmx.c" "${GEN_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SKEL_SO}"
        COMMAND "${HEXAGON_CC}"
            -m${ARCH}
            -mcpu=hexagon${ARCH}
            -mhvx=${ARCH}
            -mhvx-ieee-fp
            -shared
            -G0
            -Wl,-Bsymbolic
            -Wl,--wrap=malloc
            -Wl,--wrap=calloc
            -Wl,--wrap=free
            -Wl,--wrap=realloc
            -Wl,--wrap=memalign
            -Wl,-soname,libcdsp_peak_skel_${ARCH}.so
            -o "${SKEL_SO}"
            "${SKEL_OBJ}" "${IMP_OBJ}" "${POWER_OBJ}" "${HMX_OBJ}"
        DEPENDS "${SKEL_OBJ}" "${IMP_OBJ}" "${POWER_OBJ}" "${HMX_OBJ}"
        VERBATIM
    )

    list(APPEND DSP_SKELS "${SKEL_SO}")
endforeach()

add_custom_target(cdsp_peak_skels ALL DEPENDS ${DSP_SKELS})
add_dependencies(cdsp_peak cdsp_peak_skels)

add_custom_command(TARGET cdsp_peak POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${DSP_SKELS} "$<TARGET_FILE_DIR:cdsp_peak>"
    VERBATIM
)

set(CDSP_PEAK_CAT "")
if (WIN32 AND CDSP_PEAK_HTP_CERT)
    set(CAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/cat")
    set(CDSP_PEAK_INF "${CMAKE_CURRENT_BINARY_DIR}/cdsp_peak_skel.inf")
    set(CDSP_PEAK_CAT "${CAT_DIR}/cdsp_peak_skel.cat")
    file(TO_NATIVE_PATH "${CAT_DIR}" CAT_DIR_NATIVE)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cdsp_peak_skel.inf.in"
        "${CDSP_PEAK_INF}"
        @ONLY
    )

    file(TO_CMAKE_PATH "$ENV{WINDOWS_SDK_BIN}/arm64" WINSDK_BIN0_ARM64)
    file(TO_CMAKE_PATH "$ENV{WINDOWS_SDK_BIN}/x86" WINSDK_BIN0_X86)
    file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}/arm64" WINSDK_BIN1_ARM64)
    file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}/x86" WINSDK_BIN1_X86)
    set(WINSDK_PATHS ${WINSDK_BIN0_ARM64} ${WINSDK_BIN0_X86} ${WINSDK_BIN1_ARM64} ${WINSDK_BIN1_X86})

    find_program(INF2CAT NAMES inf2cat.exe PATHS ${WINSDK_PATHS} REQUIRED)
    find_program(SIGNTOOL NAMES signtool.exe PATHS ${WINSDK_PATHS} REQUIRED)
    set(SIGNTOOL_CERT_ARGS /f "${CDSP_PEAK_HTP_CERT}")
    if (CDSP_PEAK_HTP_CERT_PASSWORD)
        list(APPEND SIGNTOOL_CERT_ARGS /p "${CDSP_PEAK_HTP_CERT_PASSWORD}")
    endif()

    add_custom_command(
        OUTPUT "${CDSP_PEAK_CAT}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CAT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CAT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CDSP_PEAK_INF}" "${CAT_DIR}/cdsp_peak_skel.inf"
        COMMAND "${CMAKE_COMMAND}" -E copy ${DSP_SKELS} "${CAT_DIR}"
        COMMAND "${INF2CAT}" "/driver:${CAT_DIR_NATIVE}" /os:10_25H2_ARM64
        COMMAND "${SIGNTOOL}" sign /fd sha256 ${SIGNTOOL_CERT_ARGS} "${CDSP_PEAK_CAT}"
        DEPENDS "${CDSP_PEAK_INF}" ${DSP_SKELS}
        VERBATIM
    )
    add_custom_target(cdsp_peak_cat ALL DEPENDS "${CDSP_PEAK_CAT}")
    add_dependencies(cdsp_peak cdsp_peak_cat)
    add_custom_command(TARGET cdsp_peak POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CDSP_PEAK_CAT}" "$<TARGET_FILE_DIR:cdsp_peak>"
        VERBATIM
    )
elseif (WIN32)
    message(WARNING "CDSP_PEAK_HTP_CERT is not set; skel .so files will build but Windows NPU loading may require a signed cdsp_peak_skel.cat")
endif()

install(TARGETS cdsp_peak RUNTIME DESTINATION .)
install(FILES ${DSP_SKELS} DESTINATION .)
if (CDSP_PEAK_CAT)
    install(FILES "${CDSP_PEAK_CAT}" DESTINATION .)
endif()
