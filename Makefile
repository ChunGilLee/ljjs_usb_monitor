# 컴파일러 및 플래그
CC = gcc
CFLAGS = `pkg-config --cflags libusb-1.0` -I/usr/include/libevdev-1.0/
LDFLAGS = `pkg-config --libs libusb-1.0` -levdev

# 타겟 이름
DEVICE_VERIFICATION = device_verification_automove

# 소스 파일
SRC_DEVICE_VERIFICATION = device_verification.c

# 기본 빌드 규칙
all: $(DEVICE_VERIFICATION)

$(DEVICE_VERIFICATION): $(SRC_DEVICE_VERIFICATION)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# clean 규칙
clean:
	rm -f $(SRC_DEVICE_VERIFICATION)
