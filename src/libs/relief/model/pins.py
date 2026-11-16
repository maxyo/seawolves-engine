"""The float models make_ncnn_models.py converts and their pins."""
# The ncnn graphs' names (pbrify-<name>.ncnn.param / .bin) and the float models they are made of
# (fernandotonon/QtMeshEditor-models at b35b7f4c924936803bf79e868302d7f8f643d302) with their pins; the build fetches the
# same files by the same digests (src/libs/relief/CMakeLists.txt).
MODELS = {
    'pbrify-normal': ('1x-PBRify_NormalV3.onnx', '19d78f555f994219e2692876ab0fb6c3e4382503952dbe72d74db89d24870ba4'),
    'pbrify-height': ('1x-PBRify_Height.onnx', '2b14afd583148cb5b0a554bf86a009b6a898aadc00d213aa3ec2544e17c835f5'),
    'pbrify-roughness': ('1x-PBRify_RoughnessV2.onnx', '8a8de1bbeab70df8b2550bdc11fddd5a315a12eeb47d561ce55b38551ea52131'),
}
