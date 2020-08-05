#ifndef VUFS_H
#define VUFS_H
#include <vumodule.h>
#include <fcntl.h>

VU_PROTOTYPES(vufs)

#define MAX_OPEN_FILES 1024

#define O_UNLINK (O_PATH | O_EXCL)

#define VUFS_TYPEMASK 0x7
#define VUFS_MOVE 0x0
#define VUFS_MERGE 0x1
#define VUFS_COW 0x2
#define VUFS_MINCOW 0x4
#define VUFS_VSTAT 0x100

struct vufs_t {
  pthread_mutex_t mutex;

  char *source;
  char *target;
  int rdirfd;
  int vdirfd;
  int ddirfd;
  int flags;

  char *except[];
};

struct vufs_fdprivate {
  FILE *getdentsf;
  char path[];
};

int open_fds[MAX_OPEN_FILES];

#endif
