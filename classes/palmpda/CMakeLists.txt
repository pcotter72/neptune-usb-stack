# palmpda.class - Palm PDA serial/HotSync -> usbpalm.device. Dual library (NepSerialBase class +
# NepSerDevBase device) + MUI GUI (libbase MUI base, like printer).
add_neptune_class(palmpda BASETYPE NepSerialBase GUI SOURCES dev.c DEFINES HAS_LIBOPEN DEPS sana2_headers)
