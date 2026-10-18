.PHONY: all clean cd

all:
	$(MAKE) -C kext
	$(MAKE) -C mount

clean:
	$(MAKE) -C kext clean
	$(MAKE) -C mount clean

# guest additions CD image (as root)
cd:
	sh dist/mkcd.sh
