# serialpl2303.class - USB serial (Prolific PL2303) -> usbmodem.device. No GUI; embeds the
# usbmodem.device (dev.c, MakeLibrary'd in libInit). See add_neptune_class() in the top CMakeLists.
add_neptune_class(serialpl2303 BASETYPE NepSerialBase SOURCES dev.c)
