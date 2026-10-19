# NeptuneModuleLayout.cmake - nep_module_layout(<target> [WRITABLE])
#
# Link <target> with the shared freestanding-module layout script (ldscripts/module.lds).
#
# A .library/.class is not an ordinary executable: LoadSeg() starts execution at offset 0 of
# the first hunk, the romtag's RT_ENDSKIP must bound the module (exec's resident scan
# resumes there), and a ROM module may contain no writable data at all.  Those
# are placement facts, and placement is the linker's job - leaving it to the order of the
# add_executable() source list is what left every module here with the debug put-char helper,
# not doNotExecute, at offset 0.
#
# WRITABLE waives the no-writable-section assertion for a module that is only ever LoadSeg'd
# and never placed in ROM.  The serial backend is waived wholesale: debug.lib carries a
# writable _SysBase, so a serial build of a ROM module can never be clean - that build is a
# debugging aid, not a shippable ROM, and a -serial archive ships no ROM/ drawer.
#
# Usage:
#   include(cmake/NeptuneModuleLayout.cmake)
#   nep_module_layout(<target> [WRITABLE])

get_filename_component(_nep_module_ldscript
    "${CMAKE_CURRENT_LIST_DIR}/../ldscripts/module.lds" ABSOLUTE)
set(_NEP_MODULE_LDSCRIPT "${_nep_module_ldscript}"
    CACHE INTERNAL "freestanding module layout script")
unset(_nep_module_ldscript)

set(_NEP_LAYOUT_CHECK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/NeptuneModuleLayoutCheckScript.cmake"
    CACHE INTERNAL "nep_module_layout entry-stub check")

function(nep_module_layout target)
    cmake_parse_arguments(ARG "WRITABLE" "" "" ${ARGN})
    if(NOT EXISTS "${_NEP_MODULE_LDSCRIPT}")
        message(FATAL_ERROR
            "nep_module_layout(${target}): layout script not found at ${_NEP_MODULE_LDSCRIPT}")
    endif()
    target_link_options(${target} PRIVATE "-Wl,-T,${_NEP_MODULE_LDSCRIPT}")
    if(ARG_WRITABLE OR NEPTUNE_DEBUG_BACKEND STREQUAL "serial")
        target_link_options(${target} PRIVATE "-Wl,--defsym,__nep_writable_ok=1")
    endif()
    # The script places .text.entry first, but only if something is in it: assert the
    # linked module really starts with the stub, so a new class that forgets the attribute
    # fails here rather than executing the debug helper when someone runs it.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:${target}>
            -DEXPECT=70ff4e75
            -P ${_NEP_LAYOUT_CHECK_SCRIPT}
        COMMENT "Layout check: ${target} must start with the do-not-execute stub"
        VERBATIM)
    # Relink when the contract changes.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_NEP_MODULE_LDSCRIPT}")
endfunction()
