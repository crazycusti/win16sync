CLIENT_TARGET = w16sync.exe
CLIENT_OBJS = main.o proto.o
SETUP_TARGET = setup.exe
SETUP_OBJS = setup.o
CFLAGS = -q -bt=windows -ml -zWs -3

.c.o:
	wcc $(CFLAGS) $<

all: $(CLIENT_TARGET) $(SETUP_TARGET) .SYMBOLIC

w16sync.res: w16sync.rc resource.h
	wrc -q -bt=windows -r w16sync.rc

$(CLIENT_TARGET): $(CLIENT_OBJS) w16sync.res
	wlink option quiet system windows name $(CLIENT_TARGET) file main.o file proto.o
	wrc -q -bt=windows w16sync.res $(CLIENT_TARGET)

setup.res: setup.rc setupres.h $(CLIENT_TARGET)
	wrc -q -bt=windows -r setup.rc

$(SETUP_TARGET): $(SETUP_OBJS) setup.res
	wlink option quiet system windows name $(SETUP_TARGET) file setup.o library ddeml
	wrc -q -bt=windows setup.res $(SETUP_TARGET)

clean: .SYMBOLIC
	rm -f $(CLIENT_OBJS) $(SETUP_OBJS) w16sync.res setup.res $(CLIENT_TARGET) $(SETUP_TARGET) *.err *.map
