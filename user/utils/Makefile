ARCH?= $(shell uname -m | sed -e s/i.86/x86_32/)
LIB_POSTFIX      = $(shell echo $(ARCH) | sed -e s/x86_// | grep -v 32 | grep -v Power)

SOURCE_FILES     := $(wildcard *.c)
OBJECT_FILES     := $(patsubst %.c,%.o,$(SOURCE_FILES))
HEADER_FILES     := $(wildcard *.h)

CC := gcc
##
## Build options
##

CFLAGS          += -Werror
CFLAGS          += -Wall
CFLAGS          += -Wno-unused
CFLAGS          += -D_GNU_SOURCE
CFLAGS          += -fPIC
CFLAGS          += -g

LIBS = -L/usr/lib$(LIB_POSTFIX)

MAIN_OBJ = castle-cli

##
## "all" Target
##

all: $(MAIN_OBJ)

##
## Other targets
##

$(MAIN_OBJ): $(OBJECT_FILES) $(HEADER_FILES)
	$(CC) $(CFLAGS) $(OBJECT_FILES) $(LIBS) -o $@

%.o: %.c $(HEADER_FILES)
	$(CC) $(CFLAGS) -c -o $*.o $*.c

tags: $(SOURCE_FILES) $(HEADER_FILES)
	ctags $(SOURCE_FILES) $(HEADER_FILES)

.PHONY: clean
clean:
	rm -f *.o $(MAIN_OBJ) 

.PHONY: install
install: $(MAIN_OBJ)
	install $(MAIN_OBJ) /usr/sbin