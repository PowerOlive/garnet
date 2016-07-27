// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "escher/geometry/quad.h"
#include "escher/gl/gles2/bindings.h"

namespace escher {

void DrawQuad(GLint position, const Quad& quad);

}  // namespace escher
