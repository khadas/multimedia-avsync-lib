#CC=${HOST_GCC}

#export CC BUILD_DIR STAGING_DIR TARGET_DIR
all:
	-$(MAKE) -C LOG=$(LOG) src all
install:
	-$(MAKE) -C src install
clean:
	-$(MAKE) -C src clean
