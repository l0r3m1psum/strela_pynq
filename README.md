Dependencies

  * `linux-xlnx` tag `xilinx-v2025.2`
  * GCC 13.3.0

[Xilinx/AMD are bad](https://wiki.archlinux.org/title/Xilinx_Vivado) hence we need to dedicate and entire VM to their software.


```
REM Accorting to UG973 for 2024.2 Ubuntu 24.04 is an officially supported distribution.
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
# Additional dependencies to build TVM
sudo apt install cmake g++-13-arm-linux-gnueabihf python3-venv
# For TVM-FFI
mkdir -p ~/arm-sysroot
cd ~/arm-sysroot
wget "https://ports.ubuntu.com/ubuntu-ports/pool/main/p/python3.12/libpython3.12-dev_3.12.3-1ubuntu0.13_armhf.deb"
dpkg -x libpython3.12-dev_3.12.3-1ubuntu0.13_armhf.deb .
```

```
wsl --shutdown
wsl -d Ubuntu
net use Z: "\\wsl.localhost\Ubuntu"
net use /delete Z:
```
