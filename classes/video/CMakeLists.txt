# usbvideo.class - USB Video Class (UVC 1.0/1.1/1.5) cameras -> usbvideo.device. Embeds
# the device (dev.c, MakeLibrary'd in libInit). uvc_desc.c: descriptors to model (no USB
# I/O); uvc_ctrl.c: class requests and negotiation; uvc_stream.c: the stream - the payload
# engine, start and stop - with its two transports in uvc_iso.c and uvc_bulk.c; uvc_status.c:
# the status endpoint and its events; uvc_controls.c: the standard controls by name, with
# the table of what they are; usbvideo.gui.c: the per-camera settings window (MUI).
# See add_poseidon_class() in the top-level CMakeLists.txt.
add_poseidon_class(usbvideo BASETYPE NepVideoBase GUI
    SOURCES uvc_desc.c uvc_ctrl.c uvc_controls.c uvc_stream.c uvc_iso.c uvc_bulk.c uvc_status.c dev.c usbvideo.gui.c)
# -lnix: the C library's memmove() (H.264 is slid down over the JPEG frame that carried it)
# rests on a block copy that lives there. Nothing else is taken from it. The VHI driver
# does the same.
target_link_libraries(usbvideo_class PRIVATE
    -Wl,--start-group -lc -lnix -lgcc -Wl,--end-group)
