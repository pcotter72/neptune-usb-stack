# rawwrap.class - raw USB endpoint passthrough -> usbraw.device. Dual library (class + embedded
# device via dev.c) + MUI GUI (per-instance MUI base).
add_neptune_class(rawwrap BASETYPE NepRawWrapBase GUI SOURCES dev.c DEFINES HAS_LIBOPEN)
