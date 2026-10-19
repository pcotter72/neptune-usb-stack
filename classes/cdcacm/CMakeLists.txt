# cdcacm.class - USB CDC-ACM serial (modems/OBEX) -> usbmodem.device. No GUI; embeds the
# usbmodem.device (dev.c, MakeLibrary'd in libInit). See add_neptune_class() in the top CMakeLists.
add_neptune_class(cdcacm BASETYPE NepSerialBase SOURCES dev.c)
