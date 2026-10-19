# ethwrap.class - generic SANA-II Ethernet wrapper -> usbethwrap.device. Dual library + MUI GUI.
# See add_neptune_class().
add_neptune_class(ethwrap BASETYPE NepEthBase GUI
    SOURCES dev.c DEFINES HAS_LIBOPEN DEPS sana2_headers)
