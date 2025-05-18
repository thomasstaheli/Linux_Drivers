#ifndef ACCUMULATE_H
#define ACCUMULATE_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define ACCUMULATE_IOC_MAGIC     '+'
#define ACCUMULATE_CMD_RESET     _IO(ACCUMULATE_IOC_MAGIC, 0)
#define ACCUMULATE_CMD_CHANGE_OP _IOW(ACCUMULATE_IOC_MAGIC, 1, int)

#define OP_ADD                   0
#define OP_MULTIPLY              1

#endif /* ACCUMULATE_H */
