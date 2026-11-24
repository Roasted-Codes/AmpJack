# SPDX-License-Identifier: GPL-3.0-or-later
# Kernel entry points of xa.xex (the wireless adapter driver); the shared ones, the
# NV64_THUNK macro and its rationale are in thunks_common.s.
    .include "thunks_common.s"

    NV64_THUNK XaLinkDpc, XaLinkDpcC
    NV64_THUNK XaMatch, XaMatchC
    NV64_THUNK XaAddDevice, XaAddDeviceC
    NV64_THUNK XaRemoveDevice, XaRemoveDeviceC
    NV64_THUNK XaCloseDone, XaCloseDoneC
    NV64_THUNK XaThread, XaThreadC
