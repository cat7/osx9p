SDK	?= /Developer/SDKs/MacOSX10.4u.sdk
KH	= $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CC	= gcc-4.0
CXX	= g++-4.0
ARCH	= -arch ppc
FLAGS	= $(ARCH) -mlong-branch -force_cpusubtype_ALL -nostdinc -fno-common -msoft-float -Os -g \
	  -fno-builtin -finline -static \
	  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	  -mmacosx-version-min=10.4 -isysroot $(SDK) -I$(KH) -I. \
	  -Wall -Wno-unused-parameter
CFLAGS	= $(FLAGS) -std=gnu99
CXXFLAGS = $(FLAGS) -fno-rtti -fno-exceptions -fcheck-new -fapple-kext
LDFLAGS	= $(ARCH) -isysroot $(SDK) -mmacosx-version-min=10.4 -nostdlib \
	  -static -r -lkmodc++ -lkmod -lcc_kext

OBJS	= kmod.o vfsops.o vnops.o proto.o lock.o ids.o virtio9p.o
HDRS	= 9p.h virtio9p.h
KEXT	= osx9p.kext

all: $(KEXT)

$(KEXT): osx9p Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp Info.plist $@/Contents/
	cp osx9p $@/Contents/MacOS/

osx9p: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $<

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -rf $(KEXT) osx9p *.o
