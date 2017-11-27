PLAT:=none
PLATS:=linux mingw

BIN:=nextproxy

INSTALL_TOP?=$(CURDIR)/local
INSTALL_BIN:=$(INSTALL_TOP)/bin

INSTALL:=install -p
INSTALL_EXEC:=$(INSTALL) -m0755

MKDIR:=mkdir -p
RM:=rm -rf

.PHONY:all none $(PLATS) install uninstall clean

all:$(PLAT)

none:
	@echo "Please select a PLATFORM from these:"
	@echo "    $(PLATS)"
	@echo "then do 'make PLATFORM' to complete constructions."

$(PLATS) clean:
	@cd src && $(MAKE) $@

install:
	$(MKDIR) $(INSTALL_BIN)
	cd src && $(INSTALL_EXEC) $(BIN) $(INSTALL_BIN)

uninstall:
	cd $(INSTALL_BIN) && $(RM) $(BIN)
