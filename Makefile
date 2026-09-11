ifndef TARGET_COMPILE
    $(error TARGET_COMPILE not set, e.g. export TARGET_COMPILE=aarch64-none-elf-)
endif
ifndef KP_DIR
    KP_DIR = ../..
endif

CC = $(TARGET_COMPILE)gcc
LD = $(TARGET_COMPILE)ld

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

objs := hid_hide.o

# KPM 内核加载器逐个解析未定义符号，缺失即拒绝加载：
# -fno-stack-protector  去掉 __stack_chk_guard/__stack_chk_fail 依赖
# -fno-asynchronous-unwind-tables  去掉 .eh_frame
# -fno-common           避免 SHN_COMMON 符号被加载器拒绝
CFLAGS := -fno-stack-protector -fno-asynchronous-unwind-tables -fno-common

all: hid_hide.kpm

hid_hide.kpm: ${objs}
	${CC} -r -o $@ $^

%.o: %.c
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -Thid_hide.lds -c -O2 -o $@ $<

.PHONY: clean
clean:
	rm -rf *.kpm
	find . -name "*.o" | xargs rm -f
