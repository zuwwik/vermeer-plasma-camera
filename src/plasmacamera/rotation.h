// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

inline int cameraDisplayRotation(int mount, int device, bool frontFacing)
{
    const int rotation = mount + (frontFacing ? -device : device);
    return (rotation % 360 + 360) % 360;
}
