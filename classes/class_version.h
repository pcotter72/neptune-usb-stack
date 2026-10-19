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

/* About requester of the class settings windows: name, version, the fork
 * statement and the application's MUIA_Application_Copyright (the class's own
 * authors). extra is a string literal appended after it, "" for none. Needs MUI
 * in scope at the call site. */
#define NEP_CLASS_ABOUT_EXTRA(app, win, extra) \
    do { \
        IPTR _copyright = (IPTR) ""; \
        get((app), MUIA_Application_Copyright, &_copyright); \
        MUI_RequestA((app), (win), 0, NULL, "OK", \
                     NEP_NAME_VER(CLASS_NAME) " (" NEPTUNE_DATE ")\n\n" NEP_FORK_TXT "\n\n%s" extra, \
                     &_copyright); \
    } while(0)
#define NEP_CLASS_ABOUT(app, win) NEP_CLASS_ABOUT_EXTRA(app, win, "")

#endif /* NEPTUNE_CLASS_VERSION_H */
