OBJ = avsync.c tsync.c queue.c pattern.c log.c

TARGET = libamlavsync.so
#TEST = avsync_test

# rules
all: $(TARGET)
#all: $(TARGET) $(TEST)

LD_FLAG = -lm -lpthread

$(TARGET): $(OBJ)
	$(CC) $(TARGET_CFLAGS)  -g -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib $(LD_FLAG) $(OBJ) -shared -fPIC -o $@

#$(TEST): $(TARGET) test.c
#	cp $(TARGET) $(STAGING_DIR)/usr/lib/
#	$(CC) $(TARGET_CFLAGS)  -g -D_FILE_OFFSET_BITS=64 -Wall -I$(STAGING_DIR)/usr/include/ -L$(STAGING_DIR)/usr/lib -lamlavsync test.c -o $@

.PHONY: clean

clean:
	rm -f *.o $(TARGET) $(TEST)

install:
	cp aml_avsync_log.h $(STAGING_DIR)/usr/include/
	cp aml_avsync.h $(STAGING_DIR)/usr/include/
	cp $(TARGET) $(TARGET_DIR)/usr/lib/
#	cp $(TEST) $(TARGET_DIR)/usr/bin/
