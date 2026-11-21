CC      = gcc
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu11 -Iinclude -Isrc/comun -pthread
LDFLAGS = -pthread

COMUN   = src/comun/config.o src/comun/errores.o
DSMD    = $(patsubst %.c,%.o,$(wildcard src/dsmd/*.c))
LIBDSM  = src/libdsm/api.o src/libdsm/pager.o
APPS    = apps/matmul apps/coherencia apps/video apps/contador

all: dsmd dsmctl libdsm.a $(APPS)

dsmd: $(DSMD) $(COMUN)
	$(CC) $(LDFLAGS) -o $@ $^

libdsm.a: $(LIBDSM) $(COMUN)
	ar rcs $@ $^

dsmctl: tools/dsmctl.o $(COMUN)
	$(CC) $(LDFLAGS) -o $@ $^

apps/%: apps/%.o libdsm.a
	$(CC) $(LDFLAGS) -o $@ $^ -lm

%.o: %.c include/*.h src/comun/config.h src/dsmd/dsmd.h src/libdsm/libdsm.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*/*.o tools/*.o apps/*.o dsmd dsmctl libdsm.a $(APPS)

.PHONY: all clean
