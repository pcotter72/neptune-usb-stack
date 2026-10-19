/* class_version.h - the class $VER cookie, shared by common.h (the class bodies)
 * and class_main.c (the romtag id-string).
 *
 * Requires CLASS_NAME from the class's CMakeLists (-D). The version numbers are the
 * distribution's (NEPTUNE_VERSION/REVISION, injected globally by the top-level
 * CMakeLists) - the class fleet ships in lockstep, so no class states its own.
 */
#ifndef NEPTUNE_CLASS_VERSION_H
#define NEPTUNE_CLASS_VERSION_H

#include <neptune_version.h>

#define VERSION_STRING  NEP_VER(CLASS_NAME)

#endif /* NEPTUNE_CLASS_VERSION_H */
