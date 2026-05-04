Dependencies

  * `linux-xlnx` tag `xilinx-v2025.2`
  * GCC 13.3.0

[Xilinx/AMD are bad](https://wiki.archlinux.org/title/Xilinx_Vivado) hence we need to dedicate and entire VM to their software.


```
REM Accorting to UG973 for 2024.2 Ubuntu 22.04 is an officially supported distribution.
curl -O "https://cloud-images.ubuntu.com/wsl/jammy/current/ubuntu-jammy-wsl-amd64-ubuntu22.04lts.rootfs.tar.gz"
md D:\WSL\Ubuntu
wsl --import Ubuntu D:\WSL\Ubuntu ubuntu-jammy-wsl-amd64-wsl.rootfs.tar.gz
```

```
adduser your_username
usermod -aG sudo your_username
echo [user] >>/etc/wsl.conf
echo default=your_username >>/etc/wsl.conf
su your_username
# Dowload and extract xsetup for Vitis 2024.2
./xsetup -b ConfigGen # Choose option 3. Vitis Embedded Development
./xsetup -b AuthTokenGen
sudo mkdir -p /tools/Xilinx
sudo chown -R $USER:$USER /tools/Xilinx
chmod -R 755 /tools/Xilinx
./xsetup --agree XilinxEULA,3rdPartyEULA --batch Install --config ~/.Xilinx/install_config.txt
sudo /tools/Xilinx/Vitis/2024.2/scripts/installLibs.sh
sudo apt install x11-utils
sudo locale-gen en_US.UTF-8
sudo apt install build-essential flex bison gcc-13-arm-linux-gnueabihf bc device-tree-compiler
```

```
wsl --shutdown
wsl -d Ubuntu
net use Z: "\\wsl.localhost\Ubuntu"
```
