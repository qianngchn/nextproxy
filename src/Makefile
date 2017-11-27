PLAT:=none
PLATS:=linux mingw

CC:=gcc -std=gnu99
CFLAGS:=-Wall -O2 $(PLATCFLAGS)
LDFLAGS:=$(PLATLDFLAGS)
SRCS:=main.c event.c socket.c
OBJS:=$(SRCS:%.c=%.o)

BIN:=nextproxy

ALL:=$(BIN)

STRIP:=strip -s
MKDIR:=mkdir -p
RM:=rm -rf

.PHONY:all none $(PLATS) clean

all:$(PLAT)

none:
	@echo "Please select a PLATFORM from these:"
	@echo "    $(PLATS)"
	@echo "then do 'make PLATFORM' to complete constructions."

linux:
	@$(MAKE) $(ALL)

mingw:
	@$(MAKE) $(ALL) "PLATCFLAGS=-static" "PLATLDFLAGS=-lregex -lws2_32"

%.o:%.c
	$(CC) $^ $(CFLAGS) -c -o $@

$(BIN):$(OBJS)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -o $@
	$(STRIP) $@

clean:
	$(RM) $(OBJS)
	$(RM) $(ALL)
