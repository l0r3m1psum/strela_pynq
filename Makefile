KDIR ?= $(PWD)/3rdparty/linux-xlnx
ccflags-y += -I$(src)/include
obj-m += src/cgra_dma.o

all:
	make -C $(KDIR) M=$(PWD) modules
	$(CC) -g -Iinclude src/test_bypass.c -o src/test_bypass

clean:
	make -C $(KDIR) M=$(PWD) clean
