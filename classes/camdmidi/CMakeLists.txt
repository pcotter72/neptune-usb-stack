# camdusbmidi.class - USB-MIDI -> CAMD. Single library, no config GUI (nothing to set: CAMD
# names its ports itself). At bind time its subtask writes a tiny CAMD MIDI driver to DEVS:Midi/<id>
# (name[] patched to the id, NPorts to the cable count, rewritten only when the image differs);
# that driver opens this class back and calls its two EXTRA library vectors usbCAMDOpenPort@-90 /
# usbCAMDClosePort@-96, and the two sides share one struct CAMDAdapter per open port (camdusbmidi.h) with
# an RX and a TX hook. camd_vectors.h injects the vectors into the shared skeleton's funcTable.
add_neptune_class(camdusbmidi BASETYPE NepMidiBase AMIGALIB)    # amiga.lib: CallHookA
target_compile_definitions(camdusbmidi_class PRIVATE CLASS_VECTORS_HDR="camd_vectors.h")

# ---------------------------------------------------------------------------------------------
# The embedded CAMD MIDI driver, built from source (camd/camd_driver.c) instead of a frozen blob.
#
#   camdusbmidi.sfd --(sfdc)--> proto/camdusbmidi.h   (inline usbCAMD* stubs the driver calls)
#   camd_driver.c --(ld, ldscripts/module.lds)--> camd_driver   (a CAMD-shaped HUNK exe: the doNotExecute
#                                          entry stub@0, MidiDeviceData@4)
#   camd_driver --(gen_camddriver.py)--> CAMDDriver.c (ULONG[] + CAMDDriver_NAMEOFFSET/NAMESIZE/
#                                          NPORTSOFFSET: where name[] and NPorts are, how much
#                                          of name[] may be patched)
#   CAMDDriver.c is #included by camdusbmidi.class.c.
# ---------------------------------------------------------------------------------------------

# (a) generate proto/inline/clib headers for the class's exported usbCAMD* vectors
generate_sfd_headers(camdusbmidi CAMD_SFD_GEN)

# (b) build the driver as a CAMD-driver-shaped HUNK exe (entry stub first, MidiDeviceData at +4)
add_executable(camd_driver camd/camd_driver.c)
add_dependencies(camd_driver camdusbmidi_sfd_headers)
# -Os: this is never shipped as an executable - it is linked, turned into a ULONG[] by
# gen_camddriver.py below and embedded inside camdusbmidi.class, so its size is class size.
target_compile_options(camd_driver PRIVATE -Os -ffreestanding)
target_include_directories(camd_driver PRIVATE
    ${CAMD_SFD_GEN}                         # generated proto/camdusbmidi.h
    ${CMAKE_SOURCE_DIR}/include             # vendored midi/camddevices.h + aros_compat.h
    ${CMAKE_CURRENT_SOURCE_DIR})            # camdusbmidi.h
target_link_libraries(camd_driver PRIVATE
    -Wl,--start-group -lc -lamiga -lgcc -Wl,--end-group)
target_link_options(camd_driver PRIVATE
    -nostdlib -nostartfiles -s
)

# WRITABLE: camd.library LoadSeg()s this blob, so unlike a ROM module it may carry .data/.bss.
nep_module_layout(camd_driver WRITABLE)
set_target_properties(camd_driver PROPERTIES SUFFIX "")

# (c) codegen the embedded ULONG[] + recompile-safe name offset from the linked driver
set(CAMD_BLOB_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen-blob)
set(CAMD_BLOB ${CAMD_BLOB_DIR}/CAMDDriver.c)
add_custom_command(OUTPUT ${CAMD_BLOB}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CAMD_BLOB_DIR}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/camd/gen_camddriver.py
            $<TARGET_FILE:camd_driver> ${CAMD_BLOB}
    DEPENDS camd_driver ${CMAKE_CURRENT_SOURCE_DIR}/camd/gen_camddriver.py
    VERBATIM)
add_custom_target(camd_driver_blob DEPENDS ${CAMD_BLOB})

# (d) the class #include "CAMDDriver.c" now resolves to the generated one (the checked-in copy
#     is gone); make the gen dir win and the class wait for the blob.
add_dependencies(camdusbmidi_class camd_driver_blob)
target_include_directories(camdusbmidi_class BEFORE PRIVATE ${CAMD_BLOB_DIR})
