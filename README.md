# buse_fb_driver
Framebuffer sriver for BUSE displays


## Compile DT overlay
```bash
# compile overlay
dtc -@ -I dts -O dtb -o busefb.dtbo busefb-overlay.dts
# copy to overlays
sudo cp busefb.dtbo /boot/overlays/

# add to /boot/firmware/config.txt
dtoverlay=busefb
```

DT config

```bash
spi-max-frequency = <3000000>;
cs-gpios = <&gpio 17 0>;
width =  <128>;       // Display width in pixels
height = <19>;       // Display height in pixels
panels = <4>;        // Number of display panels

```


## Build kernel module and install
```bash
# build
make
# copy
sudo cp busefb.ko /lib/modules/$(uname -r)/kernel/drivers/misc/
# create module dependencies to enable driver
sudo depmod -a

```


