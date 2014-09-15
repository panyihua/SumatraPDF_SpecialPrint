/* Copyright 2013 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef AppPrefs_h
#define AppPrefs_h

#define PREFS_FILE_NAME         L"SumatraPDF-settings.txt"
#ifdef ENABLE_SUMATRAPDF_USER_INI
#define USER_PREFS_FILE_NAME    L"SumatraPDF-user.ini"
#endif

/* enum from windowState */
enum {
    WIN_STATE_NORMAL = 1, /* use remembered position and size */
    WIN_STATE_MAXIMIZED,  /* ignore position and size, maximize the window */
    WIN_STATE_FULLSCREEN,
    WIN_STATE_MINIMIZED,
};

#include "DisplayState.h"

extern GlobalPrefs *gGlobalPrefs;

void DeleteGlobalPrefs(GlobalPrefs *globalPrefs);

namespace prefs {

bool Load();
bool Save();
bool Reload(bool forceReload=false);

void RegisterForFileChanges();
void UnregisterForFileChanges();

namespace conv {

const WCHAR *   FromDisplayMode(DisplayMode mode);
DisplayMode     ToDisplayMode(const WCHAR *s, DisplayMode default=DM_AUTOMATIC);
void            FromZoom(char **dst, float zoom, DisplayState *stateForIssue2140=NULL);
float           ToZoom(const char *s, float default=ZOOM_FIT_PAGE);

};

};

#endif
