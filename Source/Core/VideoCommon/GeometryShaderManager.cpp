// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderManager.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DisplayXR.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

static constexpr int LINE_PT_TEX_OFFSETS[8] = {0, 16, 8, 4, 2, 1, 1, 1};

void GeometryShaderManager::Init()
{
  constants = {};

  // Init any initial constants which aren't zero when bpmem is zero.
  SetViewportChanged();
  SetProjectionChanged();

  dirty = true;
}

void GeometryShaderManager::Dirty()
{
  // This function is called after a savestate is loaded.
  // Any constants that can changed based on settings should be re-calculated
  m_projection_changed = true;

  // Uses EFB scale config
  SetLinePtWidthChanged();

  dirty = true;
}

void GeometryShaderManager::SetVSExpand(VSExpand expand)
{
  if (constants.vs_expand != expand)
  {
    constants.vs_expand = expand;
    dirty = true;
  }
}

void GeometryShaderManager::SetConstants(PrimitiveType prim)
{
  const bool follow_head = g_ActiveConfig.stereo_mode == StereoMode::DisplayXR &&
                           g_ActiveConfig.stereo_displayxr_follow_head;
  if (follow_head && m_displayxr_eyes_generation != DisplayXR::GetTrackedEyesGeneration())
  {
    m_displayxr_eyes_generation = DisplayXR::GetTrackedEyesGeneration();
    m_projection_changed = true;
  }

  if (m_projection_changed && g_ActiveConfig.stereo_mode != StereoMode::Off)
  {
    m_projection_changed = false;

    if (xfmem.projection.type == ProjectionType::Perspective)
    {
      const float offset = g_ActiveConfig.stereo_depth;
      float left = -offset;
      float right = offset;

      // Each eye's camera follows the viewer's tracked eye: the separation setting applies to an
      // average 63 mm between the eyes, and moving the head moves both cameras with the same
      // scale. The convergence plane (stereoparams[2]) does not move, so the screen plane stays
      // put while the scene shows parallax.
      const std::optional<DisplayXR::TrackedEyes> eyes =
          follow_head ? DisplayXR::GetTrackedEyes() : std::nullopt;
      if (eyes)
      {
        constexpr float HALF_AVERAGE_IPD = 0.0315f;
        left = offset * eyes->left_x / HALF_AVERAGE_IPD;
        right = offset * eyes->right_x / HALF_AVERAGE_IPD;
      }

      constants.stereoparams[0] = g_ActiveConfig.bStereoSwapEyes ? right : left;
      constants.stereoparams[1] = g_ActiveConfig.bStereoSwapEyes ? left : right;
    }
    else
    {
      constants.stereoparams[0] = constants.stereoparams[1] = 0;
    }

    constants.stereoparams[2] = g_ActiveConfig.stereo_convergence;

    dirty = true;
  }

  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    if (prim == PrimitiveType::Points)
      SetVSExpand(VSExpand::Point);
    else if (prim == PrimitiveType::Lines)
      SetVSExpand(VSExpand::Line);
    else
      SetVSExpand(VSExpand::None);
  }

  if (m_viewport_changed)
  {
    m_viewport_changed = false;

    constants.lineptparams[0] = 2.0f * xfmem.viewport.wd;
    constants.lineptparams[1] = -2.0f * xfmem.viewport.ht;

    dirty = true;
  }
}

void GeometryShaderManager::SetViewportChanged()
{
  m_viewport_changed = true;
}

void GeometryShaderManager::SetProjectionChanged()
{
  m_projection_changed = true;
}

void GeometryShaderManager::SetLinePtWidthChanged()
{
  constants.lineptparams[2] = bpmem.lineptwidth.linesize / 6.f;
  constants.lineptparams[3] = bpmem.lineptwidth.pointsize / 6.f;
  constants.texoffset[2] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.lineoff];
  constants.texoffset[3] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.pointoff];
  dirty = true;
}

void GeometryShaderManager::SetTexCoordChanged(u8 texmapid)
{
  TCoordInfo& tc = bpmem.texcoords[texmapid];
  int bitmask = 1 << texmapid;
  constants.texoffset[0] &= ~bitmask;
  constants.texoffset[0] |= tc.s.line_offset << texmapid;
  constants.texoffset[1] &= ~bitmask;
  constants.texoffset[1] |= tc.s.point_offset << texmapid;
  dirty = true;
}

void GeometryShaderManager::DoState(PointerWrap& p)
{
  p.Do(m_projection_changed);
  p.Do(m_viewport_changed);

  p.Do(constants);

  if (p.IsReadMode())
  {
    // Fixup the current state from global GPU state
    // NOTE: This requires that all GPU memory has been loaded already.
    Dirty();
  }
}
