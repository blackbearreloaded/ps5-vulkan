/* Copyright (C) 2026 BlackBearReloaded */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public-header link witness; this ELF is not a console execution test. */
#include <EGL/egl.h>
#include <vulkan/vulkan.h>

int main(void)
{
    PFN_vkCreateComputePipelines pipeline = vkCreateComputePipelines;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return pipeline == 0 || display == EGL_NO_DISPLAY;
}
