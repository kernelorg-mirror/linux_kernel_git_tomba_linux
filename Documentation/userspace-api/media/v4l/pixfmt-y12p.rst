.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _V4L2-PIX-FMT-Y12P:

******************************
V4L2_PIX_FMT_Y12P ('Y12P')
******************************

Grey-scale image as a MIPI RAW12 packed array


Description
===========

This is a packed grey-scale image format with a depth of 12 bits per
pixel. Two consecutive pixels are packed into 3 bytes. The first 2 bytes
contain the 8 high order bits of the pixels, and the 3rd byte contains the 4
least significants bits of each pixel, in the same order.

**Byte Order.**
Each cell is one byte.

.. tabularcolumns:: |p{2.2cm}|p{1.2cm}|p{1.2cm}|p{3.1cm}|


.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       2 1 1 1


    -  -  start + 0:
       -  Y'\ :sub:`00high`
       -  Y'\ :sub:`01high`
       -  Y'\ :sub:`01low`\ (bits 7--4)

          Y'\ :sub:`00low`\ (bits 3--0)

