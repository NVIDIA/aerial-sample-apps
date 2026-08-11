#!/usr/bin/env python3
"""Regression test for the NumPy component guard in CMakeLists.txt."""

import os
import re
import unittest


class TestCMakeNumpyGuard(unittest.TestCase):
    def test_numpy_component_checked(self):
        cmakelist = os.path.join(os.path.dirname(__file__), '..', 'CMakeLists.txt')
        with open(cmakelist, 'r') as f:
            content = f.read()
        # The guard must explicitly require NumPy before linking Python3::NumPy.
        self.assertIn('Python3_NumPy_FOUND', content)
        guard_pattern = re.compile(
            r'if\s*\(\s*NOT\s+Python3_FOUND\s+OR\s+NOT\s+Python3_NumPy_FOUND\s+OR\s+NOT\s+pybind11_FOUND\s*\)',
            re.IGNORECASE)
        self.assertRegex(content, guard_pattern)
