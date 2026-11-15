# Opaque loading upload budget

`nice -n 10 python3 tests/loading_upload/run.py` checks the production admission
policy and actual LoadingOpaque receipt without a renderer/GPU. Run only in an
authorized CPU slot. It covers cancelled/replaced/unadvanced/unstable receipts,
selected current-generation entries, byte/time/count boundaries, oversized
singleton progress, overflow and the unchanged two-upload gameplay cap.

This does not establish GPU completion, Android loading speed or renderer
publication. `tests/renderer_bgfx/upscale_swap_check.cpp` exercises the real
Read→Resident path with renderer-created containers and rejects wrong dimensions.
The loading batch control there preserves actual texture publication/validation;
GLES/Vulkan execution and target-device loading remain separate acceptance steps.
