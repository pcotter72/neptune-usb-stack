# lan78xx.class - USB Ethernet (Microchip LAN78xx) -> usblan78xx.device (SANA-II). Dual library, no
# GUI. See add_neptune_class().
add_neptune_class(lan78xx BASETYPE NepEthBase
    INCLUDE lan78xx.class.h SOURCES dev.c DEFINES HAS_LIBOPEN DEPS sana2_headers mui_headers)
