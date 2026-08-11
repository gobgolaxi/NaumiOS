#ifndef SHELL_H
#define SHELL_H

/* Kernel-mode task entry point: a polling UART line-editor + command
   dispatcher. Register it with task_create() like any other task — the
   preemptive scheduler means its busy-poll of the RX FIFO only costs its
   own timeslice, not anyone else's. */
void shell_task(void);

#endif
