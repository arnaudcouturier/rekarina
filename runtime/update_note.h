// The update notice (update_note.c): whether a newer release of the port exists.
#pragma once
#include <stddef.h>

// Where the newest version is asked for (update_note_win.c): the site's release feed, the file the launcher reads first
// (installer/update.py FEED), shaped like a GitHub release ("tag_name"). The GitHub repository is private, so its API
// answers 404 to anyone else.
#define UPDATE_REPO_HOST L"rekarina.dev"
#define UPDATE_REPO_PATH L"/releases/latest.json"

void update_note_start(void);   // game runs only: starts the check on a thread of its own
// 1 when a release newer than this build is known; its version ("1.0.1") into out. Any thread.
int update_note_latest(char *out, size_t cap);

// 1 when candidate ("1.0.1", "v1.0.1") is a newer version than current ("1.0.0", "1.0.0+abc").
int update_version_newer(const char *candidate, const char *current);
// The version of GitHub's "latest release" answer (its tag_name without the "v"); 0 when it has none.
int update_tag_from_release(const char *json, char *out, size_t cap);
// One HTTPS request for the newest release's version; 0 offline, on a refusal or on another system than Windows.
int update_http_latest(char *out, size_t cap);
