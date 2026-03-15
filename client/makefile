TARGET = w16sync.exe
OBJS = main.o proto.o
CFLAGS = -q -bt=windows -ml -zWs -3

.c.o:
	wcc $(CFLAGS) $<

all: $(TARGET) .SYMBOLIC

w16sync.res: w16sync.rc resource.h
	wrc -q -bt=windows -r w16sync.rc

$(TARGET): $(OBJS) w16sync.res
	wlink option quiet system windows name $(TARGET) file main.o file proto.o
	wrc -q -bt=windows w16sync.res $(TARGET)

clean: .SYMBOLIC
	rm -f $(OBJS) w16sync.res $(TARGET) *.err *.map
