# Design

The kernel driver is based on the Linux [accel subsystem](https://docs.kernel.org/accel/)
and takes inspiration from the Rockchip's NPU dirver in `drivers/accel/rocket`.

It differs from it because it needs to be efficiently driven by the [IREE](https://iree.dev)
[HAL](https://iree.dev/reference/mlir-dialects/HAL/). For this reason it needs

  * [in- and out- fences](https://docs.kernel.org/driver-api/sync_file.html#in-fences-and-out-fences)
    to support [`hal.device.queue.execute`](https://docs.kernel.org/driver-api/sync_file.html#in-fences-and-out-fences)
  * [non-coherent DMA allocation](https://docs.kernel.org/core-api/dma-api.html#part-ii-non-coherent-dma-allocations),
    because STRELA has no MMU.
  * range flush and invalidation using `PREP_BO` and `FINI_BO` because IREE
    suballocates from large buffers.

# Dependencies

  * `linux-xlnx` tag `xilinx-v2025.2`
  * GCC 13.3.0

[Xilinx/AMD are bad](https://wiki.archlinux.org/title/Xilinx_Vivado) hence we need to dedicate and entire VM to their software.

```
REM According to UG973 for 2024.2 Ubuntu 24.04 is an officially supported distribution.
curl -O "https://cdimages.ubuntu.com/ubuntu-wsl/noble/daily-live/current/noble-wsl-amd64.wsl"
md D:\WSL\Ubuntu
wsl --import Ubuntu D:\WSL\Ubuntu noble-wsl-amd64.wsl
```

```
adduser your_username
usermod -aG sudo your_username
echo >>/etc/wsl.conf
echo [user] >>/etc/wsl.conf
echo default=your_username >>/etc/wsl.conf
su your_username
# Dowload and extract xsetup for Vitis 2024.2
# FPGAs_AdaptiveSoCs_Unified_2024.2_1113_2356_Lin64.bin
./xsetup -b ConfigGen # Choose option 3. Vitis Embedded Development
./xsetup -b AuthTokenGen
sudo mkdir -p /tools/Xilinx
sudo chown -R $USER:$USER /tools/Xilinx
chmod -R 755 /tools/Xilinx
./xsetup --agree XilinxEULA,3rdPartyEULA --batch Install --config ~/.Xilinx/install_config.txt
sudo /tools/Xilinx/Vitis/2024.2/scripts/installLibs.sh
sudo apt install x11-utils unzip
sudo locale-gen en_US.UTF-8
# Dependencies for building the kernel module.
sudo apt install build-essential flex bison gcc-13-arm-linux-gnueabihf bc device-tree-compiler
# Nice to have for QEMU testing
sudo usermod -aG kvm $USER
# To make a bootable image for the Pynq
sudo apt install u-boot-tools
```

```
wsl --shutdown
wsl -d Ubuntu
net use Z: "\\wsl.localhost\Ubuntu"
net use /delete Z:
```
