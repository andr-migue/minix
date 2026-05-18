/*
 * cpu_bound.c - CPU-bound test process for scheduler penalty validation.
 *
 * This program runs an infinite arithmetic loop without ever blocking,
 * so it exhausts its quantum on every scheduling cycle. The MINIX scheduler
 * should detect this behavior and progressively lower its priority.
 *
 * Usage (inside the MINIX VM):
 *   cc -o cpu_bound cpu_bound.c
 *   ./cpu_bound &
 *   echo "cpu_bound PID: $!"
 *   top     # observe priority increasing (worsening) every 5 seconds
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
	volatile long x = 0;

	printf("cpu_bound started, PID=%d\n", getpid());
	fflush(stdout);

	/* Never yields the CPU voluntarily: exhausts every quantum. */
	while (1) {
		x++;
	}

	return 0;
}
