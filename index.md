---
layout: page
title: HOWTO
---

{:toc}

Clone and compile the Linux kernel
----------------------------------

Use some extra directory for all the components needed

    cd c2c/

Clone the repository (this will take a *few minutes*)

    c2c$ git clone https://github.com/CTU-IIG/802.11p-linux.git
    c2c$ cd 802.11p-linux

Checkout particular branch

    c2c/802.11p-linux$ git checkout its-g5_v3

Prepare the folder for compilation

    c2c/802.11p-linux$ mkdir _build

Use the appropriate defconfig

    c2c/802.11p-linux$ make O=_build x86_64_defconfig

Configure the kernel if necessary (enable MAC80211_OCB_DEBUG,
CONFIG_MAC80211_STA_DEBUG etc.)

    c2c/802.11p-linux$ cd _build
    c2c/802.11p-linux/_build$ make menuconfig

Run the compilation

    c2c/802.11p-linux/_build$ make -j4

Install the modules and the kernel

    c2c/802.11p-linux/_build$ sudo make modules_install
    c2c/802.11p-linux/_build$ sudo make install


iw -- wifi configuration tool
----------------------------

Use some extra directory for all the components needed

    cd c2c/

Install libnl development files

    c2c$ sudo apt-get install libnl-dev

Clone the official iw repository

    c2c$ git clone https://github.com/CTU-IIG/802.11p-iw.git
    c2c$ cd 802.11p-iw
    c2c/802.11p-iw$ git checkout its-g5_v3

Build it

    c2c/802.11p-iw$ make

Install it

    c2c/802.11p-iw$ sudo PREFIX=/ make install

Test it

    c2c/802.11p-iw$ /sbin/iw | grep -i ocb
     	dev <devname> ocb leave
     	dev <devname> ocb join <freq in MHz> <5MHZ|10MHZ> [fixed-freq]


wireless-regdb -- regulatory information
----------------------------------------

Use some extra directory for all the components needed

    cd c2c/

Install some extra packages

    c2c$ sudo apt-get install python-m2crypto

Clone the repository

    c2c$ git clone https://github.com/CTU-IIG/802.11p-wireless-regdb.git
    c2c$ cd 802.11p-wireless-regdb
    c2c/802.11p-wireless-regdb$ git checkout its-g5_v1

    c2c/802.11p-wireless-regdb$ make
    c2c/802.11p-wireless-regdb$ sudo PREFIX=/ make install


CRDA -- Central Regulatory Domain Agent
--------------------------------------

Use some extra directory for all the components needed

    cd c2c/

Install some extra packages

    c2c$ sudo apt-get install python-m2crypto
    c2c$ sudo apt-get install libgcrypt11-dev

Clone the repository

    c2c$ git clone https://github.com/CTU-IIG/802.11p-crda.git
    c2c$ cd 802.11p-crda
    c2c/802.11p-crda$ git checkout its-g5_v1

We are using our own key for regulatory.bin and CRDA

    c2c/802.11p-crda$ cp /lib/crda/pubkeys/username.key.pub.pem pubkeys/

Compile + install it

    c2c/802.11p-crda$ make
    c2c/802.11p-crda$ sudo PREFIX=/ REG_BIN=/lib/crda/regulatory.bin make install

Test CRDA + generated regulatory.bin

    c2c/802.11p-crda$ sudo /sbin/regdbdump ../802.11p-wireless-regdb/regulatory.bin | grep -i ocb
    country 00: invalid
     	(5850.000 - 5925.000 @ 20.000), (20.00), NO-CCK, OCB-ONLY


Right now is probably the right time reboot the computer into the
newly compiled kernel


Configure OCB interface
----------------------

    sudo iw reg set DE
    sudo ip link set wlan0 down
    sudo iw dev wlan0 set type ocb
    sudo ip link set wlan0 up
    sudo iw dev wlan0 ocb join 5890 10MHZ

Get the interface statistics

    ip -s link show dev wlan0
