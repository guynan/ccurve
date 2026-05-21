
CC ?= gcc
CFLAGS = -Wall -Wextra --std=c99
DEBUG_FLAGS = -Wstrict-prototypes -Wpointer-arith -Wshadow \
			 -pg -g 
LFLAGS = -lm
#$(CC) $(CFLAGS) $(DEBUG_FLAGS) dual_curve.c $(LFLAGS)

all:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) -shared -o libcurve_engine.so -fPIC dual_curve.c


blob:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) dual_curve.c  -lm

        
