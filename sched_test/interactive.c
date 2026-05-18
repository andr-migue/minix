/*
 * interactive.c - Interactive (I/O-bound) test process for scheduler validation.
 *
 * This program sleeps for one second between iterations, voluntarily yielding
 * the CPU each cycle. It never exhausts a full quantum, so the scheduler
 * should not penalize it: its priority should remain stable at USER_Q.
 *
 * Usage (inside the MINIX VM):
 *   cc -o interactive interactive.c
 *   ./interactive &
 *   echo "interactive PID: $!"
 *   top     # observe priority staying constant while cpu_bound degrades
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int tick = 0;

	printf("interactive started, PID=%d\n", getpid());
	fflush(stdout);

	/* Yields the CPU every second: never exhausts a full quantum. */
	while (1) {
		sleep(1);
		printf("tick %d (PID=%d)\n", ++tick, getpid());
		fflush(stdout);
	}

	return 0;
}
