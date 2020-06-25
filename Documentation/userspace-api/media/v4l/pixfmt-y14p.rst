.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _V4L2-PIX-FMT-Y14P:

**************************
V4L2_PIX_FMT_Y14P ('Y14P')
**************************

Grey-scale image as a MIPI RAW14 packed array


Description
===========

This is a packed grey-scale image format with a depth of 14 bits per
pixel. Every four consecutive samples are packed into seven bytes. Each
of the first four bytes contain the eight high order bits of the pixels,
and the three following bytes contains the six least significants bits of
each pixel, in the same order.

**Byte Order.**
Each cell is one byte.

.. tabularcolumns:: |p{1.8cm}|p{1.0cm}|p{1.0cm}|p{1.0cm}|p{1.1cm}|p{3.3cm}|p{3.3cm}|p{3.3cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       2 1 1 1 1 3 3 3


    -  -  start + 0:
       -  Y'\ :sub:`00high`
       -  Y'\ :sub:`01high`
       -  Y'\ :sub:`02high`
       -  Y'\ :sub:`03high`
       -  Y'\ :sub:`01low bits 1--0`\ (bits 7--6)

	  Y'\ :sub:`00low bits 5--0`\ (bits 5--0)

       -  Y'\ :sub:`02low bits 3--0`\ (bits 7--4)

	  Y'\ :sub:`01low bits 5--2`\ (bits 3--0)

       -  Y'\ :sub:`03low bits 5--0`\ (bits 7--2)

	  Y'\ :sub:`02low bits 5--4`\ (bits 1--0)
