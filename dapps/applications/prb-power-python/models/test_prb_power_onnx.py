"""Regression test for prb_power_onnx importability without torch."""

import builtins
import sys


def test_import_without_torch():
    """The module must import even when torch is not installed."""
    # Hide any already-loaded torch modules.
    saved_modules = {}
    for name in list(sys.modules.keys()):
        if name == 'torch' or name.startswith('torch.'):
            saved_modules[name] = sys.modules.pop(name)

    real_import = builtins.__import__

    def mock_import(name, globals=None, locals=None, fromlist=(), level=0):
        if name == 'torch' or name.startswith('torch.'):
            raise ImportError(f"No module named '{name}'")
        return real_import(name, globals, locals, fromlist, level)

    try:
        builtins.__import__ = mock_import
        # Force a fresh import of the module under test.
        if 'prb_power_onnx' in sys.modules:
            del sys.modules['prb_power_onnx']
        import prb_power_onnx
        assert prb_power_onnx.TORCH_AVAILABLE is False
        assert hasattr(prb_power_onnx, 'InferenceModel')
    finally:
        builtins.__import__ = real_import
        sys.modules.update(saved_modules)
        if 'prb_power_onnx' in sys.modules:
            del sys.modules['prb_power_onnx']


if __name__ == '__main__':
    test_import_without_torch()
    print('test_import_without_torch passed')
