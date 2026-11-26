add_library(videoogl
  OGLBoundingBox.cpp
  OGLBoundingBox.h
  OGLConfig.cpp
  OGLConfig.h
  OGLGfx.cpp
  OGLGfx.h
  OGLMain.cpp
  OGLNativeVertexFormat.cpp
  OGLPerfQuery.cpp
  OGLPerfQuery.h
  OGLPipeline.cpp
  OGLPipeline.h
  OGLShader.cpp
  OGLShader.h
  OGLStreamBuffer.cpp
  OGLStreamBuffer.h
  OGLTexture.cpp
  OGLTexture.h
  OGLVertexManager.cpp
  OGLVertexManager.h
  ProgramShaderCache.cpp
  ProgramShaderCache.h
  SamplerCache.cpp
  SamplerCache.h
  VideoBackend.h
)

target_link_libraries(videoogl PUBLIC common videocommon)

if(ANDROID)
  target_sources(videoogl PRIVATE
    OGLDisplayXR.cpp
    OGLDisplayXR.h
  )
  target_include_directories(videoogl PRIVATE ${CMAKE_SOURCE_DIR}/Externals/DisplayXR/include)
  target_link_libraries(videoogl PRIVATE androidcommon android GLESv3 EGL)
endif()
if(X11_FOUND)
  target_link_libraries(videoogl PRIVATE PkgConfig::X11)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(videoogl PRIVATE use_pch)
endif()
