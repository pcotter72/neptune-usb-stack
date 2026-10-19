#ifndef NEPTUNE_PATHS_H
#define NEPTUNE_PATHS_H
/*
 * On-disk locations, one definition each, shared by the library, NeptuneUSB,
 * the CLI tools and the ROM startup.
 */

#define NEP_HWDRAWER      "DEVS:USBHardware"            /* host-controller drivers */
#define NEP_CLASSDRAWER   "DEVS:USBClasses"             /* class drivers */
#define NEP_PRESETDRAWER  "SYS:Prefs/Presets/Neptune"   /* attach/detach sounds */
#define NEP_PREFS_ENV     "ENV:Sys/neptune.prefs"
#define NEP_PREFS_ENVARC  "ENVARC:Sys/neptune.prefs"

/* Poseidon locations. Read only by the one-time prefs import: when no
   neptune.prefs exists, poseidon.prefs is loaded, the drawer prefixes
   below are rewritten to the ones above (see pMigrateCfgPaths()) and the
   result is saved as neptune.prefs. */
#define NEP_OLDPREFS_ENV      "ENV:Sys/poseidon.prefs"
#define NEP_OLDPREFS_ENVARC   "ENVARC:Sys/poseidon.prefs"
#define NEP_OLDCLASSDRAWER    "SYS:Classes/USB"
#define NEP_OLDPRESETDRAWER   "SYS:Prefs/Presets/Poseidon"

#endif /* NEPTUNE_PATHS_H */
