%~dp0/../glslc.exe -fshader-stage=comp --target-env=vulkan1.3 %~dp0/bloom_downsample.glsl -O -o %~dp0/../../../install/shader/bloom_downsample.comp.spv
%~dp0/../glslc.exe -fshader-stage=comp --target-env=vulkan1.3 %~dp0/bloom_upscale.glsl -O -o %~dp0/../../../install/shader/bloom_upscale.comp.spv