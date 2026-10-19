# bootmouse.class - USB boot-protocol mouse -> input.device. MUI config GUI.
# See add_neptune_class() in the top-level CMakeLists.txt.
add_neptune_class(bootmouse BASETYPE NepHidBase GUI DEFINES HAS_LIBOPEN)
