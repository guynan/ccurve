
CC ?= gcc
CFLAGS = -Wall -Wextra --std=c99
DEBUG_FLAGS = -Wstrict-prototypes -Wpointer-arith -Wshadow \
			 -pg -g 
LFLAGS = -lm
#$(CC) $(CFLAGS) $(DEBUG_FLAGS) dual_curve.c $(LFLAGS)

C_FILES = dual_curve.c date_utils.c file_utils.c xccy_swap.c

all:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) -shared -o libcurve_engine.so -fPIC $(C_FILES)


blob:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) $(C_FILES)  -lm

        
