KDIR ?= $(PWD)/../3rdparty/linux-xlnx

obj-m += strela.o
strela-y := strela_driver.o
ccflags-y += -I$(src)/../include/uapi

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
