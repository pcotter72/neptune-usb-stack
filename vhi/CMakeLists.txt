# usbvideo.vhi - the VHI driver for USB video cameras. VHI applications (cAMIra, VHI Studio)
# list LIBS:VHI/ and open what they find there. This driver has no USB code: it is a client
# of usbvideo.device, the application interface of usbvideo.class.
#
# A freestanding library laid out like the classes (entry stub, romtag, no writable data),
# with its own five vectors instead of the class skeleton's.
add_executable(usbvideo_vhi usbvideo.vhi.c)

target_compile_options(usbvideo_vhi PRIVATE -Os -ffreestanding)
target_compile_definitions(usbvideo_vhi PRIVATE __NOLIBBASE__)
target_include_directories(usbvideo_vhi PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR})           # mjpegdht.h, which the UVC tools use as well
# -lnix: the C library's memmove() (the frame is slid down over itself) rests on a block
# copy that lives there. Nothing else is taken from it.
target_link_libraries(usbvideo_vhi PRIVATE
    -Wl,--start-group -lc -lnix -lgcc -Wl,--end-group)
target_link_options(usbvideo_vhi PRIVATE -nostdlib -nostartfiles -s)

set_target_properties(usbvideo_vhi PROPERTIES OUTPUT_NAME usbvideo SUFFIX ".vhi")
nep_enable_lto(usbvideo_vhi)
nep_module_layout(usbvideo_vhi)            # entry at 0, RT_ENDSKIP, no writable data
