/* updater.h - checks GitHub for a newer release and installs it (see updater.c). */
#ifndef UPDATER_H
#define UPDATER_H

enum { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_FAILED };

void updater_init(int argc, char **argv);   /* at start: finds the program's folder, removes *.old leftovers */
void updater_check_start(void);              /* background check for a newer release */
void updater_install_start(void);            /* download and install the newer release (in the background) */
int updater_state(void);                     /* UPD_* */
const char *updater_message(void);           /* the current status, for the start screen */
const char *updater_version(void);           /* this build's version, e.g. "0.2.9" */
const char *updater_new_version(void);       /* the newer release's version */
void updater_restart(void);                  /* start the updated program and end this one */

#endif
