# printer.class - USB printer (Centronics/IEEE-1284 class) -> usbparallel.device. Dual library
# (class NepPrinterBase + embedded usbparallel.device via dev.c) with a MUI config GUI.
# See add_neptune_class() in the top-level CMakeLists.txt.
add_neptune_class(printer BASETYPE NepPrinterBase GUI
    SOURCES dev.c DEFINES HAS_LIBOPEN)
