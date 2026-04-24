KDIR ?= $(PWD)/3rdparty/linux-xlnx
obj-m += src/hello.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
