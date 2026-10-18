SDK	?= /Developer/SDKs/MacOSX10.4u.sdk
CC	= gcc-4.0
CFLAGS	= -arch ppc -isysroot $(SDK) -mmacosx-version-min=10.4 -Os -Wall

mount_9p: mount_9p.c ../kext/9p.h
	$(CC) $(CFLAGS) -o $@ mount_9p.c -framework CoreFoundation

clean:
	rm -f mount_9p
