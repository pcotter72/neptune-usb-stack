# serialcp210x.class - USB serial (SiLabs CP210x) -> usbmodem.device. No GUI; embeds the
# usbmodem.device (dev.c, MakeLibrary'd in libInit). See add_neptune_class() in the top CMakeLists.
add_neptune_class(serialcp210x BASETYPE NepSerialBase SOURCES dev.c)
