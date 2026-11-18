// The problem report (runtime/report.c): one zip a player can attach to a report, made by the menu's "Make a problem
// report", by `rekarina --make-report` (the launcher's button) and offered after a run that ended in an error. Nothing is
// uploaded.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes <game folder>/reports/rekarina-report-<date>-<time>.zip: system.txt (OS, CPU, RAM, GPU and driver, the device
// profile, the build and game versions, the settings in effect, the shader cache's state), the logs of the last five
// runs (each its last 16 MiB), rekarina.ini, the setup's record, the mods folder's names, and README.txt saying what is
// inside. No save data and no game files; the user's name and home folder in any path are replaced. path (n bytes)
// receives the zip's path. 1 on success.
int report_make(char *path, size_t n);
// The folder the reports go into (<game folder>/reports, made if missing). 0 on failure.
int report_folder(char *out, size_t n);
// 1 when the previous run's log (logs/rekarina.1.log at this start) ended with an error: a crash (FATAL), a lost GPU
// device or a failed start. Read once.
int report_last_run_failed(void);
// `rekarina --make-report`: makes the report without starting the game, prints its path, shows the folder; exit code.
int report_main(void);
// At the end of a crash or a fatal error of a game run that writes logs/rekarina.log (a player's run, not a test run
// with its own log file): a message box that says the game stopped and how to make a report. Once; Windows only.
void report_crash_notice(void);

#ifdef __cplusplus
}
#endif
