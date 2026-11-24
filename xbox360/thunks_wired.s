# SPDX-License-Identifier: GPL-3.0-or-later
# Kernel entry points of xw.xex (wired controllers); the shared ones, the NV64_THUNK
# macro and its rationale are in thunks_common.s.
    .include "thunks_common.s"

    NV64_THUNK XwLinkDpc, XwLinkDpcC
    NV64_THUNK XwMatch, XwMatchC
    NV64_THUNK XwAddDevice, XwAddDeviceC
    NV64_THUNK XwRemoveDevice, XwRemoveDeviceC
    NV64_THUNK XwInDone, XwInDoneC
    NV64_THUNK XwOutDone, XwOutDoneC
    NV64_THUNK XwCloseDone, XwCloseDoneC
    NV64_THUNK XwCtrlDone, XwCtrlDoneC
    NV64_THUNK XwThread, XwThreadC
# headset audio: isochronous completions take (block, status[], length[])
    NV64_THUNK XwIsoInDone, XwIsoInDoneC
    NV64_THUNK XwIsoOutDone, XwIsoOutDoneC
    NV64_THUNK XwIsoCloseDone, XwIsoCloseDoneC
