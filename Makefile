
CC ?= gcc
CFLAGS = -Wall -Wextra --std=c99
DEBUG_FLAGS = -Wstrict-prototypes -Wpointer-arith -Wshadow \
			 -pg -g 
LFLAGS = -lm
#$(CC) $(CFLAGS) $(DEBUG_FLAGS) dual_curve.c $(LFLAGS)

C_FILES = dual_curve.c interp.c date_utils.c file_utils.c xccy_swap.c

BLPAPI_HOME ?= /opt/blpapi_cpp_3.x.x.0

all:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) -shared -o libcurve_engine.so -fPIC $(C_FILES) -lm

blob:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) $(C_FILES) -lm

bloomberg:
	g++ -std=c++17 -O2 -Wall -fPIC -shared \
	  -I$(BLPAPI_HOME)/include -L$(BLPAPI_HOME)/lib \
	  -o libbloomberg_fetcher.so \
	  blpapi_fetcher.cpp blpapi_data_mapper.cpp dual_curve.c interp.c date_utils.c \
	  -lblpapi3 -lm

        
