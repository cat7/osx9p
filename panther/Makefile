# osx9p for Mac OS X 10.3: the pre-KPI vnode layer, the shared protocol
# and transport, built with gcc 3.3 against the 10.3.9 SDK
SDK	?= /Developer/SDKs/MacOSX10.3.9.sdk
KH	= $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CC	= gcc-3.3
CXX	= g++-3.3
ARCH	= -arch ppc
FLAGS	= $(ARCH) -mlong-branch -force_cpusubtype_ALL -nostdinc -fno-common -msoft-float -Os -g \
	  -fno-builtin -finline -static \
	  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPANTHER \
	  -I$(KH) -I$(KH)/bsd -I. -I../kext \
	  -Wall -Wno-unused-parameter
CFLAGS	= $(FLAGS) -std=gnu99
CXXFLAGS = $(FLAGS) -fno-rtti -fno-exceptions -fcheck-new -fapple-kext
LDFLAGS	= $(ARCH) -nostdlib -static -r -L$(SDK)/usr/lib -lkmodc++ -lkmod

OBJS	= kmod.o vfsops.o vnops.o attrlist.o adouble.o proto.o lock.o ids.o virtio9p.o shift64.o
HDRS	= ../kext/9p.h ../kext/virtio9p.h panther.h node.h
KEXT	= osx9p.kext

export MACOSX_DEPLOYMENT_TARGET = 10.3

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

%.o: ../kext/%.c $(HDRS)
	$(CC) $(CFLAGS) -c $<

%.o: ../kext/%.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -rf $(KEXT) osx9p *.o
