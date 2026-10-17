all:
	$(MAKE) -C kext
	$(MAKE) -C mount

clean:
	$(MAKE) -C kext clean
	$(MAKE) -C mount clean
