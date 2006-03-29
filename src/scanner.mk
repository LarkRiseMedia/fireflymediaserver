CC=gcc
CFLAGS := $(CFLAGS) -g -I/opt/local/include -DHAVE_CONFIG_H -I. -I..
LDFLAGS := $(LDFLAGS) -L/opt/local/lib -lid3tag -logg -lvorbisfile -lFLAC -lvorbis -ltag_c
TARGET = scanner
OBJECTS=scanner-driver.o restart.o err.o scan-wma.o scan-aac.o scan-wav.o scan-flac.o scan-ogg.o scan-mp3.o scan-url.o scan-mpc.o os-unix.o conf.o ll.o

$(TARGET):	$(OBJECTS)
	$(CC) -o $(TARGET) $(LDFLAGS) $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(TARGET)
