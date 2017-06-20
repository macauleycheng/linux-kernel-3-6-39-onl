1. How to build kernel Debian Package
Ref: https://www.debian.org/releases/jessie/i386/ch08s06.html.en
Install: fakeroot, kernel-package
RUN on kernel folder: fakeroot make-kpkg --initrd --revision=1.0.custom kernel_image
NOTE: you can use help to see makd-kpkg argument explain.
      kernel_image
      a. If you want to modify the depency, plase modify the /usr/share/kernel-package/Control
      b. If you want to change the script after install debian packaget, please modify
      ------------------------------------------------------------------------------------
       cp /usr/share/kernel-package/examples/etc/kernel/postinst.d/initramfs \
          /etc/kernel/postinst.d/
       cp /usr/share/kernel-package/examples/etc/kernel/postrm.d/initramfs \
          /etc/kernel/postrm.d/
      ------------------------------------------------------------------------------------
      more info, please read /usr/share/kernel-package/docs/README

2. For Opx kernel Image:
   Beacause the OPX use initramfs and generated when install OPX, so it need add the initramfs-tools packages.
   I change the debian package depency as below: vi /usr/share/kernel-package/Control
   --------------------------------------------------------------------
   Package: =ST-image-=V=SA
   Architecture: =A
   Section: kernel
   Priority: optional
   Provides: =ST-image,  =ST-image-=CV, =ST-modules-=CV
   Pre-Depends: debconf (>= 0.2.17) | debconf-2.0
   Depends: kmod | module-init-tools, linux-base (>= 3~), debconf (>= 0.5) | debconf-2.0, initramfs-tools (>= 0.110~) | linux-initramfs-tool
   Recommends: firmware-linux-free (>= 3~), irqbalance
   Suggests: linux-doc-3.16, debian-kernel-handbook, grub-pc | grub-efi | extlinux
   Breaks: at (<< 3.1.12-1+squeeze1), initramfs-tools (<< 0.110~)
   -----------------------------------------------------------------------------
   
3. vi /etc/kernel-img.conf, you can man kernel-imag.conf to what it is.
