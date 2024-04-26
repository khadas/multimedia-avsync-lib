OBJ = avsync.c queue.c pattern.c log.c msync_util.c pcr_monitor.c

TARGET = libamlavsync.so
TEST = avsync_test
PCR_TEST = pcr_test

OUT_DIR ?= .
$(info "OUT_DIR : $(OUT_DIR)")
CC_FLAG += -I$(OUT_DIR)
#BUILD_TEST = yes

# rules

ifeq ($(BUILD_TEST), yes)
all: $(TEST) $(PCR_TEST)
else
all: $(TARGET)
endif

CC_FLAG += -Wall
LD_FLAG = -lm -lpthread

ifeq ($(LOG), LOGCAT)
$(info use logcat)
LD_FALG += -llog
CC_FLAG += -DENABLE_LOGCAT
endif
ifeq ($(LOG), SYSLOG)
$(info use syslog)
CC_FLAG += -DENABLE_SYSLOG
endif

VERSION_FILE := ${OUT_DIR}/aml_version.h

# If the version file is not exist, use the original file
ifeq ($(wildcard $(VERSION_FILE)),)
    $(shell cp ./aml_version.h.in ${VERSION_FILE})
endif



$(TARGET): $(OBJ)
	$(CC) $(TARGET_CFLAGS) $(CC_FLAG) -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib $(LD_FLAG) $(OBJ) -shared -Wl,-soname,$(TARGET) -fPIC -o $(OUT_DIR)/$@

$(TEST): $(TARGET) test.c
	cp $(TARGET) $(STAGING_DIR)/usr/lib/
	$(CC) $(TARGET_CFLAGS) $(CC_FLAG) -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib -lamlavsync test.c -o $(OUT_DIR)/$@

$(PCR_TEST): $(TARGET) pcr_test.c
	cp $(TARGET) $(STAGING_DIR)/usr/lib/
	$(CC) $(TARGET_CFLAGS) $(CC_FLAG) -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib -lpthread -lamlavsync pcr_test.c -o $(OUT_DIR)/$@

.PHONY: clean

clean:
	rm -f *.o $(OUT_DIR)/$(TARGET) $(OUT_DIR)/$(TEST) $(OUT_DIR)/$(PCR_TEST)
	rm ${OUT_DIR}/aml_version.h

install:
	cp aml_avsync_log.h $(STAGING_DIR)/usr/include/
	cp aml_avsync.h $(STAGING_DIR)/usr/include/
	cp aml_queue.h $(STAGING_DIR)/usr/include/
	cp $(OUT_DIR)/$(TARGET) $(TARGET_DIR)/usr/lib/
ifeq ($(BUILD_TEST), yes)
	cp $(OUT_DIR)/$(TEST) $(TARGET_DIR)/usr/bin/
	cp $(OUT_DIR)/$(PCR_TEST) $(TARGET_DIR)/usr/bin/
endif

$(shell mkdir -p $(OUT_DIR))