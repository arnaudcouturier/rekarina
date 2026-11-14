// The update notice (update_note.c): whether a newer release of the port exists.
#pragma once
#include <stddef.h>

// The repository whose releases are asked for the newest version (update_note_win.c).
#define UPDATE_REPO_HOST L"api.github.com"
#define UPDATE_REPO_PATH L"/repos/hooxielooxie/Rekarina/releases/latest"

void update_note_start(void);   // game runs only: starts the check on a thread of its own
// 1 when a release newer than this build is known; its version ("1.0.1") into out. Any thread.
int update_note_latest(char *out, size_t cap);

// 1 when candidate ("1.0.1", "v1.0.1") is a newer version than current ("1.0.0", "1.0.0+abc").
int update_version_newer(const char *candidate, const char *current);
// The version of GitHub's "latest release" answer (its tag_name without the "v"); 0 when it has none.
int update_tag_from_release(const char *json, char *out, size_t cap);
// One HTTPS request for the newest release's version; 0 offline, on a refusal or on another system than Windows.
int update_http_latest(char *out, size_t cap);
