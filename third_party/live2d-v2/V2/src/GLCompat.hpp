#pragma once
// LOCAL ADDITION (see ../../PATCHES.md).
//
// Upstream includes glad's <GL/glew.h> directly, which pins the renderer to a
// desktop OpenGL compatibility profile. GTK's GtkGLArea will not hand out a
// compatibility context, so the desktop pet needs the GLES path; the shaders
// were already written for it (they carry `#ifdef GL_ES / precision mediump`),
// only the `#version` line and the loader differ.
//
// Define LIVE2D_GLES to link GLES 3 directly - no loader needed, since GLES
// entry points are exported as real symbols by libGLESv2.
#if defined(LIVE2D_GLES)
#  include <GLES3/gl3.h>
#else
#  include <GL/glew.h>  // glad-generated desktop loader shipped in ../../Glad
#endif
