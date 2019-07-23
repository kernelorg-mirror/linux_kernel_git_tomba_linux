.. SPDX-License-Identifier: GPL-2.0

=====================
Kernel driver i2c-atr
=====================

Author: Luca Ceresoli <luca@lucaceresoli.net>

Description
-----------

An I2C Address Translator (ATR) is a device with an I2C slave parent
("upstream") port and N I2C master child ("downstream") ports, and
forwards transactions from upstream to the appropriate downstream port
with a modified slave address. The address used on the parent bus is
called the "alias" and is (potentially) different from the physical
slave address of the child bus. Address translation is done by the
hardware.

An ATR looks similar to an i2c-mux except:
 - the address on the parent and child busses can be different
 - there is normally no need to select the child port; the alias used on the
   parent bus implies it

The ATR functionality can be provided by a chip with many other
features. This file provides a helper to implement an ATR within your
driver.

The ATR creates a new I2C "child" adapter on each child bus. Adding
devices on the child bus ends up in invoking the driver code to select
an available alias. Maintaining an appropriate pool of available aliases
and picking one for each new device is up to the driver implementer. The
ATR maintains an table of currently assigned alias and uses it to modify
all I2C transactions directed to devices on the child buses.

A typical example follows.

Topology::

                      Slave X @ 0x10
              .-----.   |
  .-----.     |     |---+---- B
  | CPU |--A--| ATR |
  `-----'     |     |---+---- C
              `-----'   |
                      Slave Y @ 0x10

Alias table:

.. table::

   ======   =====
   Client   Alias
   ======   =====
   X        0x20
   Y        0x30
   ======   =====

Transaction:

 - Slave X driver sends a transaction (on adapter B), slave address 0x10
 - ATR driver rewrites messages with address 0x20, forwards to adapter A
 - Physical I2C transaction on bus A, slave address 0x20
 - ATR chip propagates transaction on bus B with address translated to 0x10
 - Slave X chip replies on bus B
 - ATR chip forwards reply on bus A
 - ATR driver rewrites messages with address 0x10
 - Slave X driver gets back the msgs[], with reply and address 0x10

Usage:

 1. In your driver (typically in the probe function) add an ATR by
    calling i2c_atr_new() passing your attach/detach callbacks
 2. When the attach callback is called pick an appropriate alias,
    configure it in your chip and return the chosen alias in the
    alias_id parameter
 3. When the detach callback is called, deconfigure the alias from
    your chip and put it back in the pool for later usage

I2C ATR functions and data structures
-------------------------------------

.. kernel-doc:: include/linux/i2c-atr.h
