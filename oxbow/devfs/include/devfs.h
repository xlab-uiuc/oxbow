#ifndef _DEVFS_H_
#define _DEVFS_H_

void init_devfs(void);
void exit_devfs(void);
int start_devfs(void);

extern int terminate;

#endif
