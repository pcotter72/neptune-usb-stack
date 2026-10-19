# moschipeth.class - USB Ethernet (MosChip MCS78xx) -> usbmoschipeth.device (SANA-II). Dual
# library + MUI GUI. See add_neptune_class().
add_neptune_class(moschipeth BASETYPE NepEthBase GUI
    SOURCES dev.c DEFINES HAS_LIBOPEN DEPS sana2_headers)
