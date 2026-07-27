# RK3568 AI Security Camera
# Source the SDK first: source /home/rrn/3568/3568_sdk/environment-setup
# Then: make

SDK_PATH ?= /home/rrn/3568/3568_sdk
SYSROOT  ?= $(SDK_PATH)/aarch64-buildroot-linux-gnu/sysroot
CC       ?= aarch64-buildroot-linux-gnu-gcc
STRIP    ?= aarch64-buildroot-linux-gnu-strip

CFLAGS  := -Wall -O2 -g
CFLAGS  += --sysroot=$(SYSROOT)
CFLAGS  += -I$(SYSROOT)/usr/include
CFLAGS  += -I$(SYSROOT)/usr/include/rockchip
CFLAGS  += -I$(SYSROOT)/usr/include/rga
CFLAGS  += -I$(SYSROOT)/usr/include/rknn
CFLAGS  += -I$(SYSROOT)/usr/include/libdrm
CFLAGS  += -I$(SYSROOT)/usr/include/EGL
CFLAGS  += -I$(SYSROOT)/usr/include/opencv4
CXXFLAGS := $(CFLAGS) -std=c++11 -Wno-sign-compare
CFLAGS  += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

LDFLAGS := --sysroot=$(SYSROOT)
LDFLAGS += -L$(SYSROOT)/usr/lib
LIBS    := -lrockchip_mpp -lrknnrt -lrga -ldrm -lpthread -lrt -ldl -lm
LIBS    += -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lmali -lmali-hook
LIBS    += -lturbojpeg
LIBS    += -lopencv_core -lopencv_imgproc -lopencv_imgcodecs
LIBS    += -lstdc++

SRCDIR   := src
CSRCS    := capture.c display.c encoder.c fisheye_mesh.c fisheye_project.c main.c pipeline.c xdg-shell-client.c
CXXSRCS  := inference.cc postprocess.cc
OBJS     := $(patsubst %.c, build/%.o, $(CSRCS)) $(patsubst %.cc, build/%.o, $(CXXSRCS))
TARGET   := rk3568_camera

.PHONY: all clean push deploy hdmi_record_switcher

all: build $(TARGET)

build:
	mkdir -p build

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true
	@echo "=== Build OK: $(TARGET) ==="

build/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: $(SRCDIR)/%.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET) tools/hdmi_record_switcher

hdmi_record_switcher:
	$(CC) $(CFLAGS) tools/hdmi_record_switcher.c -o tools/hdmi_record_switcher \
		$(LDFLAGS) \
		-L$(SYSROOT)/usr/lib \
		-I$(SYSROOT)/usr/include/gstreamer-1.0 \
		-I$(SYSROOT)/usr/include/glib-2.0 \
		-I$(SYSROOT)/usr/lib/glib-2.0/include \
		-lgstreamer-1.0 -lgobject-2.0 -lglib-2.0
	$(STRIP) --strip-unneeded tools/hdmi_record_switcher 2>/dev/null || true
	@echo "=== Build OK: tools/hdmi_record_switcher ==="

# Push to device via ADB
push: $(TARGET) tools/isolated_yolov5_test
	adb push $(TARGET) /userdata/$(TARGET)
	adb shell chmod +x /userdata/$(TARGET)
	adb push tools/isolated_yolov5_test /userdata/isolated_yolov5_test
	adb shell chmod +x /userdata/isolated_yolov5_test
	@echo "=== Deployed to /userdata/ ==="

# Build isolated test (requires SDK sourced)
tools/isolated_yolov5_test:
	cd tools && make -f Makefile.isolated
