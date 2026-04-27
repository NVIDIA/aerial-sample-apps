#!/usr/bin/env python3

#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""
Model management tool for the PRB Power dApp (Triton C API).

All models are generated at startup (via start_services.sh) and loaded
by Triton when the dApp initializes.

Load/unload operations (-m, -a, -u) require gRPC and are only available
in the prb-power-triton-grpc variant. Use -s to show model status.
"""

import argparse
import os
import sys
from typing import List, Tuple

GRPC_ONLY_MSG = (
    "This operation requires the Triton gRPC/HTTP API and is not available "
    "with the C API in-process mode.\n"
    "Use the prb-power-triton-grpc variant for load/unload support.\n"
    "All models are loaded automatically at startup in C API mode."
)


def get_models(model_repository: str) -> List[Tuple[str, str]]:
    """Scan the model repository for valid model directories.
    Returns list of (model_name, format) tuples."""
    models = []
    if not os.path.exists(model_repository):
        return models
    for item in sorted(os.listdir(model_repository)):
        model_path = os.path.join(model_repository, item)
        if not os.path.isdir(model_path):
            continue
        config_file = os.path.join(model_path, "config.pbtxt")
        version_dir = os.path.join(model_path, "1")
        if not (os.path.exists(config_file) and os.path.exists(version_dir)):
            continue
        has_onnx = os.path.exists(os.path.join(version_dir, "model.onnx"))
        has_plan = os.path.exists(os.path.join(version_dir, "model.plan"))
        has_pt = os.path.exists(os.path.join(version_dir, "model.pt"))
        has_py = os.path.exists(os.path.join(version_dir, "model.py"))
        fmt = ("onnx" if has_onnx else
               "tensorrt" if has_plan else
               "libtorch" if has_pt else
               "python" if has_py else
               "unknown")
        models.append((item, fmt))
    return models


def main():
    parser = argparse.ArgumentParser(
        description="Model management for PRB Power dApp (Triton C API). "
                    "Only -s/--status is available; load/unload require gRPC.")
    parser.add_argument("--model-name", "-m", help="Load specific model (gRPC only)")
    parser.add_argument("--all", "-a", action="store_true",
                        help="Load all available models (gRPC only)")
    parser.add_argument("--unload", "-u", help="Unload specific model (gRPC only)")
    parser.add_argument("--status", "-s", action="store_true",
                        help="Show model status (filesystem scan)")
    parser.add_argument("--model-repository", "-r", default="/models",
                        help="Model repository path (default: /models)")
    args = parser.parse_args()

    if args.model_name or args.all or args.unload:
        print(GRPC_ONLY_MSG)
        sys.exit(1)

    if args.status:
        models = get_models(args.model_repository)
        if not models:
            print(f"No valid models found in {args.model_repository}")
            sys.exit(1)

        print(f"\n{'Model':<30} {'Format':<12}")
        print("-" * 42)
        for name, fmt in models:
            print(f"  {name:<28} {fmt}")
        print(f"\n{len(models)} model(s) in {args.model_repository}")
        print("(All models loaded at startup by Triton)")
        return

    parser.print_help()


if __name__ == "__main__":
    main()