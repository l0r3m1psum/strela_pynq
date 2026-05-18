KDIR ?= $(PWD)/3rdparty/linux-xlnx
ccflags-y += -I$(src)/include

obj-m += src/strela.o

all: test
	make -C $(KDIR) M=$(PWD) modules

test:
	$(CC) -g -Iinclude src/test_bypass.c -o src/test_bypass

clean:
	make -C $(KDIR) M=$(PWD) clean
