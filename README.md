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
## Build kernel module and install
```bash
# build
make
# copy
sudo cp busefb.ko /lib/modules/$(uname -r)/kernel/drivers/misc/
# create module dependencies to enable driver
sudo depmod -a

```


