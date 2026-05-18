# Makefile for scheduler validation test programs.
# Build inside the MINIX VM with: make
# Then run both programs and observe priorities with top.

CC = cc
CFLAGS = -Wall

all: cpu_bound interactive

cpu_bound: cpu_bound.c
	$(CC) $(CFLAGS) -o cpu_bound cpu_bound.c

interactive: interactive.c
	$(CC) $(CFLAGS) -o interactive interactive.c

clean:
	rm -f cpu_bound interactive
