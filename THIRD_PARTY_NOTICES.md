# Third-party notices

This file collects the notices that must accompany binary distributions of
this firmware. Other third-party code in the repository carries its notice in
the header of the file that contains it.

## Texas Instruments — MSP430 JTAG reference code (SLAU320, Replicator430Xv2)

Applies to firmware built with `CONFIG_PROBE_MSP430_SBW=y` (the default), which
contains code ported from, and binary funclets copied from, TI's "MSP430
Programming With the JTAG Interface" source package:

- `lib/msp430/src/cpuxv2.cpp` (JTAG control sequences)
- `lib/msp430/src/sbw_tap.cpp` (Spy-Bi-Wire entry sequences and shift framing)
- `lib/msp430/src/funclets.cpp` (flash write and erase funclets, unchanged)

```
Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:

   Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the
   distribution.

   Neither the name of Texas Instruments Incorporated nor the names of
   its contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
