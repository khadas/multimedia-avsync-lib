OBJ = avsync.c queue.c pattern.c log.c msync_util.c pcr_monitor.c

TARGET = libamlavsync.so
TEST = avsync_test
PCR_TEST = pcr_test

#BUILD_TEST = yes

# rules

ifeq ($(BUILD_TEST), yes)
all: $(TEST) $(PCR_TEST)
else
all: $(TARGET)
endif

LD_FLAG = -lm -lpthread

$(TARGET): $(OBJ)
	$(CC) $(TARGET_CFLAGS)  -g -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib $(LD_FLAG) $(OBJ) -shared -fPIC -o $@

$(TEST): $(TARGET) test.c
	cp $(TARGET) $(STAGING_DIR)/usr/lib/
	$(CC) $(TARGET_CFLAGS)  -g -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib -lamlavsync test.c -o $@

$(PCR_TEST): $(TARGET) pcr_test.c
	cp $(TARGET) $(STAGING_DIR)/usr/lib/
	$(CC) $(TARGET_CFLAGS)  -g -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib -lpthread -lamlavsync pcr_test.c -o $@

.PHONY: clean

clean:
	rm -f *.o $(TARGET) $(TEST)

install:
	cp aml_avsync_log.h $(STAGING_DIR)/usr/include/
	cp aml_avsync.h $(STAGING_DIR)/usr/include/
	cp aml_queue.h $(STAGING_DIR)/usr/include/
	cp $(TARGET) $(TARGET_DIR)/usr/lib/
ifeq ($(BUILD_TEST), yes)
	cp $(TEST) $(TARGET_DIR)/usr/bin/
	cp $(PCR_TEST) $(TARGET_DIR)/usr/bin/
endif
