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

import onnx
import numpy as np
from typing import Dict, List
import argparse
import os
import shutil
import sys
import subprocess

def analyze_onnx_model_safe(model_path: str) -> Dict:
    """Analyze ONNX model WITHOUT creating ONNX Runtime session to avoid hanging."""
    
    print(f"Analyzing model: {model_path}")
    print(f"File size: {os.path.getsize(model_path) / (1024*1024):.2f} MB")
    
    try:
        # Load with ONNX (this is safe)
        print("Loading model with ONNX...")
        model = onnx.load(model_path)
        print("✓ ONNX model loaded successfully")
        
        # Validate the model (this is safe)
        print("Validating ONNX model...")
        try:
            onnx.checker.check_model(model)
            print("✓ ONNX model validation passed")
        except Exception as e:
            print(f"⚠ ONNX model validation warning: {e}")
            print("Continuing anyway...")
        
        # Extract info directly from ONNX model (avoiding ONNX Runtime)
        print("Extracting model information from ONNX graph...")
        
        inputs = []
        for input_info in model.graph.input:
            # Extract shape information
            shape = []
            if input_info.type.tensor_type.shape:
                for dim in input_info.type.tensor_type.shape.dim:
                    if dim.dim_value:
                        shape.append(dim.dim_value)
                    elif dim.dim_param:
                        shape.append(dim.dim_param)  # Keep parameter name
                    else:
                        shape.append(-1)  # Dynamic dimension
            
            # Extract type information
            type_mapping = {
                1: 'tensor(float)',   # FLOAT
                2: 'tensor(uint8)',   # UINT8
                3: 'tensor(int8)',    # INT8
                6: 'tensor(int32)',   # INT32
                7: 'tensor(int64)',   # INT64
                9: 'tensor(bool)',    # BOOL
                10: 'tensor(float16)', # FLOAT16
                11: 'tensor(double)',  # DOUBLE
            }
            tensor_type = type_mapping.get(input_info.type.tensor_type.elem_type, 'tensor(float)')
            
            inputs.append({
                'name': input_info.name,
                'shape': shape,
                'type': tensor_type
            })
        
        outputs = []
        for output_info in model.graph.output:
            # Similar extraction for outputs
            shape = []
            if output_info.type.tensor_type.shape:
                for dim in output_info.type.tensor_type.shape.dim:
                    if dim.dim_value:
                        shape.append(dim.dim_value)
                    elif dim.dim_param:
                        shape.append(dim.dim_param)
                    else:
                        shape.append(-1)
            
            tensor_type = type_mapping.get(output_info.type.tensor_type.elem_type, 'tensor(float)')
            
            outputs.append({
                'name': output_info.name,
                'shape': shape,
                'type': tensor_type
            })
        
        print("✓ Successfully extracted info from ONNX model")
        
        # Show additional model info
        print(f"\nModel metadata:")
        print(f"  IR version: {model.ir_version}")
        print(f"  Producer: {model.producer_name} {model.producer_version}")
        print(f"  Domain: {model.domain}")
        print(f"  Model version: {model.model_version}")
        print(f"  Doc string: {model.doc_string}")
        
        return {
            'inputs': inputs,
            'outputs': outputs,
            'model_path': model_path
        }
            
    except Exception as e:
        raise Exception(f"Failed to analyze model: {e}")

# Copy necessary functions from original script (to avoid import issues)
def print_model_analysis(model_info: Dict):
    """Print detailed model analysis."""
    print("=" * 60)
    print("ONNX MODEL ANALYSIS")
    print("=" * 60)
    
    print(f"Model Path: {model_info['model_path']}")
    print()
    
    print("INPUTS:")
    print("-" * 40)
    for i, input_info in enumerate(model_info['inputs'], 1):
        print(f"  Input {i}:")
        print(f"    Name: {input_info['name']}")
        print(f"    Shape: {input_info['shape']}")
        print(f"    Type: {input_info['type']}")
        print()
    
    print("OUTPUTS:")
    print("-" * 40)
    for i, output_info in enumerate(model_info['outputs'], 1):
        print(f"  Output {i}:")
        print(f"    Name: {output_info['name']}")
        print(f"    Shape: {output_info['shape']}")
        print(f"    Type: {output_info['type']}")
        print()

def onnx_type_to_triton_type(onnx_type: str) -> str:
    """Convert ONNX data type to Triton data type."""
    type_mapping = {
        'tensor(float)': 'TYPE_FP32',
        'tensor(float16)': 'TYPE_FP16',
        'tensor(float32)': 'TYPE_FP32',
        'tensor(double)': 'TYPE_FP64',
        'tensor(int8)': 'TYPE_INT8',
        'tensor(int16)': 'TYPE_INT16',
        'tensor(int32)': 'TYPE_INT32',
        'tensor(int64)': 'TYPE_INT64',
        'tensor(uint8)': 'TYPE_UINT8',
        'tensor(uint16)': 'TYPE_UINT16',
        'tensor(uint32)': 'TYPE_UINT32',
        'tensor(uint64)': 'TYPE_UINT64',
        'tensor(bool)': 'TYPE_BOOL',
        'tensor(string)': 'TYPE_STRING'
    }
    return type_mapping.get(onnx_type, 'TYPE_FP32')

def format_shape_for_triton(shape: List) -> List[int]:
    """Format shape for Triton config, handling dynamic dimensions."""
    formatted_shape = []
    for dim in shape:
        if isinstance(dim, str) or dim == -1:
            # Dynamic dimension
            formatted_shape.append(-1)
        else:
            formatted_shape.append(dim)
    
    # Remove batch dimension (first dimension) for Triton
    if len(formatted_shape) > 1:
        return formatted_shape[1:]
    else:
        return formatted_shape

def generate_triton_config(model_info: Dict, model_name: str, use_tensorrt: bool = False, use_e3_naming: bool = True, custom_input_names: Dict[str, str] = None, custom_output_names: Dict[str, str] = None, max_batch_size: int = 8) -> str:
    """Generate Triton config.pbtxt content."""
    
    platform = "tensorrt_plan" if use_tensorrt else "onnxruntime_onnx"
    
    config_lines = [
        f'name: "{model_name}"',
        f'platform: "{platform}"',
        f'max_batch_size: {max_batch_size}',
        ''
    ]
    
    # Add inputs
    for input_info in model_info['inputs']:
        shape = format_shape_for_triton(input_info['shape'])
        triton_type = onnx_type_to_triton_type(input_info['type'])
        
        # Determine input name with priority: custom > E3 naming > original
        original_name = input_info["name"]
        input_name = original_name
        
        # Apply custom naming for both ONNX and TensorRT (TensorRT uses renamed ONNX)
        # First check for custom name mapping
        if custom_input_names and original_name in custom_input_names:
            input_name = custom_input_names[original_name]
        # Then check for E3 naming convention
        elif use_e3_naming and original_name in ["input", "input__0", "INPUT"]:
            input_name = "iq_samples"
        
        config_lines.extend([
            'input [',
            '  {',
            f'    name: "{input_name}"',
            f'    data_type: {triton_type}',
            f'    dims: [ {", ".join(map(str, shape))} ]',
            '  }',
            ']',
            ''
        ])
    
    # Add outputs
    for output_info in model_info['outputs']:
        shape = format_shape_for_triton(output_info['shape'])
        triton_type = onnx_type_to_triton_type(output_info['type'])
        
        # Determine output name with priority: custom > E3 naming > original
        original_name = output_info["name"]
        output_name = original_name
        
        if custom_output_names and original_name in custom_output_names:
            output_name = custom_output_names[original_name]
        elif use_e3_naming and original_name in ["output", "output__0", "OUTPUT"]:
            output_name = "predictions"
        
        config_lines.extend([
            'output [',
            '  {',
            f'    name: "{output_name}"',
            f'    data_type: {triton_type}',
            f'    dims: [ {", ".join(map(str, shape))} ]',
            '  }',
            ']',
            ''
        ])
    
    # Add instance group for GPU
    config_lines.extend([
        'instance_group [',
        '  {',
        '    count: 1',
        '    kind: KIND_GPU',
        '  }',
        ']'
    ])
    
    return '\n'.join(config_lines)

def convert_onnx_to_tensorrt(onnx_path: str, output_path: str, model_info: Dict, precision: str = "fp16", custom_input_names: Dict[str, str] = None, custom_output_names: Dict[str, str] = None, max_batch_size: int = 8) -> bool:
    """Convert ONNX model to TensorRT engine using Python TensorRT API."""
    
    try:
        import tensorrt as trt
    except ImportError:
        print("✗ TensorRT Python module not found!")
        print("  Please ensure TensorRT is installed in the container/environment.")
        print("  You can install it manually with:")
        print("    pip install tensorrt")
        return False
    
    print(f"Converting ONNX to TensorRT (precision: {precision})...")
    print("This may take a few minutes...")
    
    # If custom input/output names are provided, create a modified ONNX model
    temp_onnx_path = onnx_path
    if custom_input_names or custom_output_names:
        print("Renaming inputs/outputs in ONNX model for TensorRT compatibility...")
        temp_onnx_path = onnx_path + ".temp_renamed.onnx"
        if not rename_onnx_inputs(onnx_path, temp_onnx_path, custom_input_names, custom_output_names):
            print("✗ Failed to rename ONNX inputs/outputs")
            return False
    
    # Create TensorRT logger and builder
    TRT_LOGGER = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(TRT_LOGGER)
    config = builder.create_builder_config()
    
    # Set memory pool size (2GB)
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
    
    # Set precision flags
    if precision == "fp16":
        config.set_flag(trt.BuilderFlag.FP16)
    elif precision == "int8":
        config.set_flag(trt.BuilderFlag.INT8)
    # fp32 is default
    
    # Create network definition
    network = builder.create_network(1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    
    # Parse ONNX model (potentially renamed)
    parser = trt.OnnxParser(network, TRT_LOGGER)
    
    with open(temp_onnx_path, 'rb') as model:
        if not parser.parse(model.read()):
            print("✗ Failed to parse ONNX model")
            for error in range(parser.num_errors):
                print(parser.get_error(error))
            # Clean up temp file if it exists
            if temp_onnx_path != onnx_path and os.path.exists(temp_onnx_path):
                os.remove(temp_onnx_path)
            return False
    
    # Handle dynamic shapes - create optimization profile
    profile = builder.create_optimization_profile()
    
    for input_info in model_info['inputs']:
        # Use custom name if available, otherwise original name
        original_name = input_info['name']
        input_name = original_name
        if custom_input_names and original_name in custom_input_names:
            input_name = custom_input_names[original_name]
        
        shape = input_info['shape']
        
        # Convert shape to concrete values
        min_shape = []
        opt_shape = []
        max_shape = []
        
        for i, dim in enumerate(shape):
            if isinstance(dim, str) or dim == -1:
                # Dynamic dimension
                if i == 0:  # Batch dimension
                    min_shape.append(1)
                    opt_shape.append(max_batch_size)
                    max_shape.append(max_batch_size * 4)  # Allow up to 4x max batch size
                else:
                    # For other dimensions, use conservative defaults
                    min_shape.append(1)
                    opt_shape.append(4)
                    max_shape.append(16)
            else:
                # Static dimension
                min_shape.append(dim)
                opt_shape.append(dim)
                max_shape.append(dim)
        
        # Set shape for this input
        profile.set_shape(input_name, tuple(min_shape), tuple(opt_shape), tuple(max_shape))
        print(f"  Input '{input_name}' shape profile:")
        print(f"    Min: {min_shape}")
        print(f"    Opt: {opt_shape}")
        print(f"    Max: {max_shape}")
    
    config.add_optimization_profile(profile)
    
    # Build the engine
    print("Building TensorRT engine...")
    serialized_engine = builder.build_serialized_network(network, config)
    
    if serialized_engine is None:
        print("✗ Failed to build TensorRT engine")
        return False
    
    # Save the engine
    with open(output_path, 'wb') as f:
        f.write(serialized_engine)
    
    print("✓ TensorRT conversion successful!")
    # Clean up temp file if it exists
    if temp_onnx_path != onnx_path and os.path.exists(temp_onnx_path):
        os.remove(temp_onnx_path)
    
    # Get engine size from the saved file
    engine_size_mb = os.path.getsize(output_path) / (1024*1024)
    print(f"  Engine size: {engine_size_mb:.2f} MB")
    return True

def rename_onnx_inputs(input_path: str, output_path: str, input_mapping: Dict[str, str] = None, output_mapping: Dict[str, str] = None) -> bool:
    """Rename inputs and outputs in an ONNX model to match desired names."""
    try:
        # Load the ONNX model
        model = onnx.load(input_path)
        
        # Rename inputs
        if input_mapping:
            for input_info in model.graph.input:
                if input_info.name in input_mapping:
                    old_name = input_info.name
                    new_name = input_mapping[old_name]
                    print(f"  Renaming input: '{old_name}' → '{new_name}'")
                    input_info.name = new_name
        
        # Rename outputs  
        if output_mapping:
            for output_info in model.graph.output:
                if output_info.name in output_mapping:
                    old_name = output_info.name
                    new_name = output_mapping[old_name]
                    print(f"  Renaming output: '{old_name}' → '{new_name}'")
                    output_info.name = new_name
        
        # Update node references
        all_mapping = {}
        if input_mapping:
            all_mapping.update(input_mapping)
        if output_mapping:
            all_mapping.update(output_mapping)
            
        if all_mapping:
            for node in model.graph.node:
                for i, name in enumerate(node.input):
                    if name in all_mapping:
                        node.input[i] = all_mapping[name]
                for i, name in enumerate(node.output):
                    if name in all_mapping:
                        node.output[i] = all_mapping[name]
        
        onnx.save(model, output_path)
        return True
        
    except Exception as e:
        print(f"Error renaming ONNX inputs/outputs: {e}")
        return False

def create_triton_model_repository(model_path: str, output_dir: str, model_name: str, 
                                 config_content: str, model_info: Dict, use_tensorrt: bool = False, 
                                 precision: str = "fp16", custom_input_names: Dict[str, str] = None, custom_output_names: Dict[str, str] = None, max_batch_size: int = 8) -> Dict:
    """Create complete Triton model repository structure."""
    
    # Add suffix to model name based on backend  
    model_name_with_suffix = f"{model_name}_trt" if use_tensorrt else f"{model_name}_onnx"
    
    # Create the main model repository directory
    repo_path = os.path.abspath(output_dir)
    model_dir = os.path.join(repo_path, model_name_with_suffix)
    version_dir = os.path.join(model_dir, "1")
    
    # Create directories
    os.makedirs(version_dir, exist_ok=True)
    
    # Handle model file based on backend
    if use_tensorrt:
        # Convert ONNX to TensorRT
        model_filename = "model.plan"
        dest_model_path = os.path.join(version_dir, model_filename)
        
        if not convert_onnx_to_tensorrt(model_path, dest_model_path, model_info, precision, custom_input_names, custom_output_names, max_batch_size):
            raise Exception("Failed to convert ONNX to TensorRT")
    else:
        # Handle ONNX model - rename inputs if custom names provided
        model_filename = "model.onnx"
        dest_model_path = os.path.join(version_dir, model_filename)
        
        if custom_input_names or custom_output_names:
            # Create renamed ONNX model
            print(f"Renaming inputs/outputs in ONNX model for Triton compatibility...")
            if not rename_onnx_inputs(model_path, dest_model_path, custom_input_names, custom_output_names):
                raise Exception("Failed to rename ONNX inputs/outputs")
        else:
            # Just copy the original model
            shutil.copy2(model_path, dest_model_path)
    
    # Write config.pbtxt
    config_path = os.path.join(model_dir, "config.pbtxt")
    with open(config_path, 'w') as f:
        f.write(config_content)
    
    return {
        'repo_path': repo_path,
        'model_dir': model_dir,
        'model_name': model_name_with_suffix,
        'config_path': config_path,
        'model_path': dest_model_path,
        'backend': 'tensorrt' if use_tensorrt else 'onnx'
    }

def main():
    parser = argparse.ArgumentParser(description='ONNX analyzer for ARM64 with TensorRT support')
    parser.add_argument('--input-model', '-i', required=True, help='Path to ONNX model file')
    parser.add_argument('--model-name', '-n', help='Model name for Triton config', default=None)
    parser.add_argument('--output-dir', '-o', help='Output directory for Triton model repository', default=None)
    parser.add_argument('--tensorrt', '-t', action='store_true',
                       help='Convert to TensorRT and create TensorRT model repository')
    parser.add_argument('--both', '-b', action='store_true',
                       help='Create both ONNX and TensorRT model repositories')
    parser.add_argument('--precision', '-p', choices=['fp32', 'fp16', 'int8'], default='fp16',
                       help='TensorRT precision mode (default: fp16)')
    parser.add_argument('--analyze-only', '-a', action='store_true', help='Only analyze model, don\'t generate config')
    parser.add_argument('--disable-e3-naming', action='store_true', 
                       help='Disable automatic renaming of generic inputs (input, input__0) to iq_samples')
    parser.add_argument('--input-names', type=str,
                       help='Custom input names as key=value pairs, e.g., "input=audio_data,input2=metadata"')
    parser.add_argument('--output-names', type=str,
                       help='Custom output names as key=value pairs, e.g., "dense=predictions,logits=scores"')
    parser.add_argument('--max-batch-size', type=int, default=8,
                       help='Maximum batch size for Triton and TensorRT optimization (default: 8)')
    
    args = parser.parse_args()
    
    # Analyze the model (safe version without ONNX Runtime)
    try:
        model_info = analyze_onnx_model_safe(args.input_model)
        print_model_analysis(model_info)
    except Exception as e:
        print(f"Error analyzing model: {e}")
        return
    
    if args.analyze_only:
        return
    
    # Get model name
    model_name = args.model_name or os.path.splitext(os.path.basename(args.input_model))[0]
    use_e3_naming = not args.disable_e3_naming
    
    # Parse custom input names
    custom_input_names = None
    if args.input_names:
        custom_input_names = {}
        for pair in args.input_names.split(','):
            if '=' in pair:
                old_name, new_name = pair.strip().split('=', 1)
                custom_input_names[old_name.strip()] = new_name.strip()
            else:
                print(f"Warning: Invalid input name pair '{pair}' - expected format 'old=new'")
        
        if custom_input_names:
            print(f"Custom input name mappings: {custom_input_names}")
    
    # Parse custom output names
    custom_output_names = None
    if args.output_names:
        custom_output_names = {}
        for pair in args.output_names.split(','):
            if '=' in pair:
                old_name, new_name = pair.strip().split('=', 1)
                custom_output_names[old_name.strip()] = new_name.strip()
            else:
                print(f"Warning: Invalid output name pair '{pair}' - expected format 'old=new'")
        
        if custom_output_names:
            print(f"Custom output name mappings: {custom_output_names}")
    
    # Determine what to create
    create_onnx = not args.tensorrt or args.both
    create_tensorrt = args.tensorrt or args.both
    
    repositories_created = []
    
    if args.output_dir:
        try:
            # Create ONNX repository
            if create_onnx:
                config_content = generate_triton_config(model_info, f"{model_name}_onnx", use_tensorrt=False, use_e3_naming=use_e3_naming, custom_input_names=custom_input_names, custom_output_names=custom_output_names, max_batch_size=args.max_batch_size)
                onnx_repo_info = create_triton_model_repository(
                    args.input_model, 
                    args.output_dir, 
                    model_name, 
                    config_content,
                    model_info,
                    use_tensorrt=False,
                    custom_input_names=custom_input_names,
                    custom_output_names=custom_output_names,
                    max_batch_size=args.max_batch_size
                )
                repositories_created.append(onnx_repo_info)
            
            # Create TensorRT repository
            if create_tensorrt:
                config_content = generate_triton_config(model_info, f"{model_name}_trt", use_tensorrt=True, use_e3_naming=use_e3_naming, custom_input_names=custom_input_names, custom_output_names=custom_output_names, max_batch_size=args.max_batch_size)
                trt_repo_info = create_triton_model_repository(
                    args.input_model, 
                    args.output_dir, 
                    model_name, 
                    config_content,
                    model_info,
                    use_tensorrt=True,
                    precision=args.precision,
                    custom_input_names=custom_input_names,
                    custom_output_names=custom_output_names,
                    max_batch_size=args.max_batch_size
                )
                repositories_created.append(trt_repo_info)
            
            # Print results
            print("\n" + "=" * 60)
            print("TRITON MODEL REPOSITORY(IES) CREATED")
            print("=" * 60)
            
            for repo_info in repositories_created:
                print(f"\n{repo_info['backend'].upper()} Repository:")
                print(f"  Repository Path: {repo_info['repo_path']}")
                print(f"  Model Directory: {repo_info['model_dir']}")
                print(f"  Config File: {repo_info['config_path']}")
                print(f"  Model File: {repo_info['model_path']}")
                print(f"  Directory Structure:")
                print(f"  {args.output_dir}/")
                print(f"  └── {repo_info['model_name']}/")
                print(f"      ├── config.pbtxt")
                print(f"      └── 1/")
                model_ext = "model.plan" if repo_info['backend'] == 'tensorrt' else "model.onnx"
                print(f"          └── {model_ext}")
                
        except Exception as e:
            print(f"Error creating model repository: {e}")
            return
    else:
        # Just show the config(s)
        print("\n" + "=" * 60)
        print("GENERATED TRITON CONFIG.PBTXT")
        print("=" * 60)
        
        if create_onnx:
            config_content = generate_triton_config(model_info, f"{model_name}_onnx", use_tensorrt=False, use_e3_naming=use_e3_naming, custom_input_names=custom_input_names, custom_output_names=custom_output_names, max_batch_size=args.max_batch_size)
            print("\nONNX Configuration:")
            print("-" * 30)
            print(config_content)
        
        if create_tensorrt:
            config_content = generate_triton_config(model_info, f"{model_name}_trt", use_tensorrt=True, use_e3_naming=use_e3_naming, custom_input_names=custom_input_names, custom_output_names=custom_output_names, max_batch_size=args.max_batch_size)
            if create_onnx:
                print("\n")
            print("TensorRT Configuration:")
            print("-" * 30)
            print(config_content)
        
        print("\nNote: Use -o/--output-dir to create complete Triton model repository structure")

if __name__ == "__main__":
    main()
