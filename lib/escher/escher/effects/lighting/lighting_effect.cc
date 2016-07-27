// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "escher/effects/lighting/lighting_effect.h"

#include <iostream>

#include "escher/gl/gles2/bindings.h"
#include "escher/rendering/canvas.h"

#include "ftl/logging.h"

namespace escher {
namespace {

constexpr bool kFilterLightingBuffer = true;

}  // namespace

LightingEffect::LightingEffect() {}

LightingEffect::~LightingEffect() {}

bool LightingEffect::Init(TextureCache* texture_cache, bool use_mipmap) {
  texture_cache_ = texture_cache;
  if (!shader_.Compile() || !blur_.Compile() ||
      !occlusion_detector_.Compile())
    return false;
  frame_buffer_ = FrameBuffer::Make();
  if (!frame_buffer_)
    return false;
  use_mipmap_ = use_mipmap;
  full_frame_ = Quad::CreateFillClipSpace(0.0f);
  return true;
}

void LightingEffect::Prepare(const Stage& stage, const Texture& depth) {
  glPushGroupMarkerEXT(24, "LightingEffect::Prepare");
  auto& size = stage.physical_size();
  frame_buffer_.Bind();

  if (!frame_buffer_.color().size().Equals(size)) {
    frame_buffer_.SetColor(use_mipmap_ ?
        texture_cache_->GetMipmappedColorTexture(size) :
        texture_cache_->GetColorTexture(size));
  }

  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(occlusion_detector_.program().id());
  glUniform1i(occlusion_detector_.depth_map(), 0);
  glUniform1i(occlusion_detector_.noise(), 1);
  auto& viewing_volume = stage.viewing_volume();
  glUniform3f(occlusion_detector_.viewing_volume(), viewing_volume.width(),
              viewing_volume.height(), viewing_volume.depth());
  auto& key_light = stage.key_light();
  glUniform4f(occlusion_detector_.key_light(), key_light.direction().x,
              key_light.direction().y, key_light.dispersion(),
              key_light.intensity());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, depth.id());

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, occlusion_detector_.noise_texture().id());

  glEnableVertexAttribArray(occlusion_detector_.position());
  DrawQuad(occlusion_detector_.position(), full_frame_);

  if (kFilterLightingBuffer) {
    // No need to make this texture mipmappable: it is for temporary storage of
    // the horizontally-filtered shadow image.
    Texture illumination = frame_buffer_.SwapColor(
        texture_cache_->GetColorTexture(size));
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(blur_.program().id());
    glUniform1i(blur_.illumination(), 0);
    glUniform2f(blur_.stride(), 1.0f / size.width(), 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, illumination.id());

    glEnableVertexAttribArray(blur_.position());
    DrawQuad(blur_.position(), full_frame_);

    illumination = frame_buffer_.SwapColor(std::move(illumination));
    glUniform2f(blur_.stride(), 0.0f, 1.0f / size.height());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, illumination.id());

    glEnableVertexAttribArray(blur_.position());
    DrawQuad(blur_.position(), full_frame_);

    if (use_mipmap_)
      GenerateMipmap(frame_buffer_.color().id());

    texture_cache_->PutTexture(std::move(illumination));
  }

  glPopGroupMarkerEXT();
}

void LightingEffect::Draw(const Texture& color) {
  glPushGroupMarkerEXT(21, "LightingEffect::Draw");

  glUseProgram(shader_.program().id());
  glUniform1i(shader_.color(), 0);
  glUniform1i(shader_.illumination(), 1);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, color.id());

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, illumination().id());

  glEnableVertexAttribArray(shader_.position());
  DrawQuad(shader_.position(), full_frame_);

  glPopGroupMarkerEXT();
}

void LightingEffect::GenerateMipmap(GLuint texture_id) const {
  glPushGroupMarkerEXT(31, "LightingEffect::GenerateMipmap");
  glBindTexture(GL_TEXTURE_2D, texture_id);
  glGenerateMipmap(GL_TEXTURE_2D);
  GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR) {
    std::cerr << "LightingEffect::GenerateMipmap() failed: "
              << gl_error << std::endl;
    FTL_DCHECK(false);
  }
  glPopGroupMarkerEXT();
}

}  // namespace escher
