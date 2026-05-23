
CC     ?= gcc
CXX    ?= g++
CFLAGS  = -Wall -Wextra --std=c99
DEBUG_FLAGS = -Wstrict-prototypes -Wpointer-arith -Wshadow \
			 -pg -g
LFLAGS  = -lm

C_DIR   = src/c
C_FILES = $(C_DIR)/dual_curve.c $(C_DIR)/interp.c $(C_DIR)/date_utils.c \
          $(C_DIR)/file_utils.c $(C_DIR)/xccy_swap.c $(C_DIR)/asset_swap.c

BLPAPI_HOME ?= /opt/blpapi_cpp_3.x.x.0

all:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) -shared -o libcurve_engine.so -fPIC \
	    -I$(C_DIR) $(C_FILES) -lm

blob:
	gcc -std=c99 -O3 $(CFLAGS) $(DEBUG_FLAGS) -I$(C_DIR) $(C_FILES) -lm

bloomberg:
	g++ -std=c++17 -O2 -Wall -fPIC -shared \
	  -I$(C_DIR) -I$(BLPAPI_HOME)/include -L$(BLPAPI_HOME)/lib \
	  -o libbloomberg_fetcher.so \
	  $(C_DIR)/blpapi_fetcher.cpp $(C_DIR)/blpapi_data_mapper.cpp \
	  $(C_DIR)/dual_curve.c $(C_DIR)/interp.c $(C_DIR)/date_utils.c \
	  -lblpapi3 -lm

# Requires: make bloomberg  (builds libbloomberg_fetcher.so first)
#           BLPAPI_HOME set to the Bloomberg C++ SDK root
# Run:      LD_LIBRARY_PATH=. ./nzd_example [YYYY-MM-DD] [host] [port]
nzd_example: all bloomberg
	$(CC) -std=c99 -O2 -Wall -I$(C_DIR) \
	  -o nzd_example examples/nzd_bloomberg_example.c \
	  -L. -lcurve_engine -lbloomberg_fetcher -lm \
	  -Wl,-rpath,'$$ORIGIN'
