KDIR ?= $(PWD)/3rdparty/linux-xlnx
ccflags-y += -I$(src)/include

obj-m += src/strela.o src/xilinx-afi.o

all: test prep_afi
	make -C $(KDIR) M=$(PWD) modules

test:
	$(CC) -g -Iinclude src/test_bypass.c -o src/test_bypass

prep_afi:
	cp $(KDIR)/drivers/fpga/xilinx-afi.c src

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f src/test_bypass src/xilinx-afi.c
