# Third-party notices

This file collects the notices that must accompany binary distributions of
this firmware. Other third-party code in the repository carries its notice in
the header of the file that contains it.

## Texas Instruments — MSP430 JTAG reference code (SLAU320, Replicator430Xv2)

Applies to firmware built with `CONFIG_PROBE_MSP430_SBW=y` (the default), which
contains code ported from, and binary funclets copied from, TI's "MSP430
Programming With the JTAG Interface" source package:

- `lib/msp430/src/cpuxv2.cpp` (JTAG control sequences)
- `lib/msp430/include/msp430/protocol.hpp` (JTAG instruction codes)
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

## Texas Instruments — MSP Debug Stack (MSPDS-OPEN-SOURCE 3.15.1.1)

Applies to firmware built with `CONFIG_PROBE_MSP430_SBW=y` (the default),
which contains code ported from TI's MSP Debug Stack open-source package
(`MSPDebugStack_OS_Package_3_15_1_1.zip`, "MSPDS"):

- `lib/msp430/src/debug.cpp`: CPUXv2 debugging (attach, context save and
  restore, registers, memory, breakpoints, single step, run, stop detection).
  Ported from the macros and helpers listed below, which it names above each
  port, and following the behaviour of the DLL files listed below.
- `lib/msp430/src/sbw_tap.cpp`: the 32-bit shift for EEM registers, after
  `_hil_2w_SetReg_XBits32`.
- `lib/msp430/include/msp430/protocol.hpp`: the `IR_EMEX_*` instruction codes,
  after `JTAG_defs.h` (the other instruction codes there come from SLAU320,
  see above), and the breakpoint count, from the device database below.

The files of the package that these were ported from, or whose behaviour they
follow, and the copyright line each carries:

- Copyright (C) 2012 Texas Instruments Incorporated, under `Bios/src/`:
  `hal/macros/SyncJtag_Conditional_SaveContextXv2.c`,
  `hal/macros/SyncJtag_AssertPor_SaveContextXv2.c`,
  `hal/macros/RestoreContext_ReleaseJtagXv2.c`,
  `hal/macros/SingleStepXv2.c`, `hal/macros/ReadAllCpuRegsXv2.c`,
  `hal/macros/WriteAllCpuRegsXv2.c`, `hal/macros/ReadMemWordsXv2.c`,
  `hal/macros/WriteMemWordsXv2.c`, `hal/macros/EemDataExchangeXv2.c`,
  `hal/macros/WaitForEem.c`, `hal/JTAG_defs.h`, `hal/EEM_defs.h`,
  `hil/edt.h`, `hil/edt.c`
- Copyright (C) 2013 Texas Instruments Incorporated:
  `Bios/src/hil/fet_generic/hil_2w.c`
- Copyright (C) 2007 - 2011 Texas Instruments Incorporated, under
  `DLL430_v3/src/`: `DLL430_OldApiV3.cpp`, and under `TI/DLL430/`:
  `RegisterAccessBase.cpp`, `CpuMemoryAccess.cpp`, `WatchdogControl.cpp`,
  `DebugManagerMSP430.cpp`, `PollingManager.cpp`,
  `EM/EemRegisters/EemRegisterAccess430.cpp`,
  `EM/EmulationManager/EmulationManager430.cpp`,
  `EM/Sequencer/Sequencer430.cpp`, `EM/Trigger/Trigger430.cpp`,
  `EM/TriggerCondition/AddressCondition430.cpp`,
  `EM/TriggerManager/TriggerManager430.cpp`,
  `EM/TriggerManager/TriggerConfigurator430.cpp`

The F5529's debug parameters (number of EEM triggers and similar) were read
from the device database of the earlier release MSPDS 3.3.1.3
(`TemplateDeviceDb`: `MSP430F55xx.cpp`, `MSP430F5xxx.h`, `MSP430Defaults.h`,
`Registration.h`), which carries "Copyright (C) 2011 Texas Instruments
Incorporated" under the same conditions. The host tests compare the probe's
JTAG traffic with transcripts derived from the same files.

All of these files carry the following notice, with the copyright line given
above for the file:

```
Copyright (C) 2007 - 2011 Texas Instruments Incorporated - http://www.ti.com/
Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/

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

Some files of the MSP Debug Stack package carry TI's TSPA licence instead (the
EnergyTrace sources, for example); nothing from those files is used.
