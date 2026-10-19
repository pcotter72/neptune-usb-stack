# bootkeyboard.class - USB boot-protocol keyboard -> input.device. MUI config GUI.
# See add_neptune_class() in the top-level CMakeLists.txt.
add_neptune_class(bootkeyboard BASETYPE NepHidBase GUI DEFINES HAS_LIBOPEN)
