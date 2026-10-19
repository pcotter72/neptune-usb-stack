/* Catalog layout version, passed to OpenCatalog() as OC_Version (locale.c). A .ct must
 * declare the same major in its `## version $VER: NeptuneUSB.catalog <maj>.<min>` line or
 * locale.library refuses it and NeptuneUSB falls back to the built-in English.
 *
 * Bump whenever neptuneusb.cd gains, loses or reorders a message: the MSG_* number is both
 * the catalog string id AND the index into CatCompArray (see locale.c), so any change to
 * the string list renumbers everything after it and makes older catalogs return the wrong
 * text. Rejecting them is the point.
 *
 * 2 -> 3: MSG_APP_VERSION and MSG_WINDOW_TITLE removed. Both carried a hardcoded version
 *         number, which now comes from project(VERSION) via NEP_VER/NEP_NAME_VER.
 * 3 -> 4: MSG_PANEL_OPTIONS_LINKPOWER and _HELP added (link power management switch).
 * 4 -> 5: MSG_PANEL_OPTIONS_BORING and _HELP added ("Make me boring" switch), plus the
 *         seven _PLAIN variants it selects between.
 * 5 -> 6: MSG_PANEL_DEVICES_EJECT and _HELP (safe-eject button) plus the three
 *         MSG_ACTION_DEV_EJECT_* requester texts added.
 * 6 -> 7: the plain wording became the only one. MSG_PANEL_OPTIONS_BORING and _HELP,
 *         MSG_POSEIDON_SHORTHELP[_PLAIN] (logo), MSG_ACTION_CFGDATA_OOPS and
 *         MSG_CFGLISTDISPATCHER_OOPS removed; each remaining *_PLAIN text moved onto
 *         its base id; MSG_REQ_OOPS_PLAIN renamed MSG_REQ_OK.
 */

#define CATALOG_VERSION  7
