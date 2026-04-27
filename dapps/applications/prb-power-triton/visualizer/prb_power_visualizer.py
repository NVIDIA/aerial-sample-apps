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
PRB Power & Inference Time Visualizer
Real-time visualization of Physical Resource Block power and inference timing metrics
"""

import zmq
import numpy as np
from flask import Flask, render_template_string, jsonify, send_file
from flask_cors import CORS
import os
import threading
import argparse
from collections import deque
import time

app = Flask(__name__)
CORS(app)

# Global state
current_data = None
last_update_time = 0
prb_history = deque(maxlen=50)  # Store last 50 PRB power snapshots
timing_history = deque(maxlen=50)  # Store last 50 timing measurements
data_lock = threading.Lock()

# HTML template with embedded JavaScript
HTML_TEMPLATE = '''
<!DOCTYPE html>
<html>
<head>
    <title>PRB Power & Inference Time Visualizer</title>
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        body { 
            font-family: Arial, sans-serif; 
            margin: 0; 
            padding: 20px; 
            background-color: #f0f0f0;
        }
        .container { 
            max-width: 1600px; 
            margin: 0 auto; 
            background: white; 
            padding: 30px; 
            border-radius: 15px; 
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            position: relative;
        }
        .nvidia-logo-top { 
            position: absolute; 
            top: 20px; 
            right: 30px; 
            width: 140px; 
            height: auto; 
        }
        .header { 
            text-align: center; 
            margin-bottom: 30px; 
            padding-bottom: 20px;
            margin-top: 15px;
            border-bottom: 3px solid #76b900;
        }
        .header h1 { 
            margin: 0 0 10px 0; 
            font-size: 2.8em; 
            color: #333; 
            font-weight: 700;
        }
        .header p { 
            margin: 0; 
            color: #666; 
            font-size: 1.1em;
        }
        .status-bar {
            text-align: center;
            margin: 15px 0 20px 0;
            font-size: 0.95em;
        }
        .status-indicator { 
            display: inline-block; 
            width: 12px; 
            height: 12px; 
            border-radius: 50%; 
            margin-right: 8px;
            animation: pulse 2s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .status-active { background: #76b900; box-shadow: 0 0 10px #76b900; }
        .status-stale { background: #f0ad4e; box-shadow: 0 0 10px #f0ad4e; }
        .status-waiting { background: #999; animation: none; }
        .status-error { background: #dc3545; animation: none; }
        .plots-grid { 
            display: grid; 
            grid-template-columns: repeat(2, 1fr); 
            gap: 25px; 
            margin-bottom: 25px;
        }
        .plot-box { 
            background: #f8f9fa; 
            padding: 20px; 
            border-radius: 10px; 
            box-shadow: 0 2px 8px rgba(0,0,0,0.1);
        }
        .plot-box h2 { 
            margin: 0 0 15px 0; 
            font-size: 1.3em; 
            color: #444; 
            font-weight: 600;
        }
        .stats-container { 
            display: grid; 
            grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); 
            gap: 15px; 
            margin-top: 25px;
        }
        .stat-card { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); 
            color: white; 
            padding: 20px; 
            border-radius: 10px; 
            text-align: center;
            box-shadow: 0 4px 15px rgba(0,0,0,0.2);
        }
        .stat-card h3 { 
            margin: 0 0 10px 0; 
            font-size: 0.95em; 
            opacity: 0.9; 
            font-weight: 500;
        }
        .stat-card p { 
            margin: 0; 
            font-size: 1.8em; 
            font-weight: 700;
        }
        .stat-card .unit {
            font-size: 0.6em;
            opacity: 0.9;
            margin-left: 4px;
        }
        .footer { 
            text-align: center; 
            margin-top: 30px; 
            padding-top: 20px; 
            border-top: 1px solid #ddd; 
            color: #666; 
            font-size: 0.85em;
        }
    </style>
</head>
<body>
    <div class="container">
        <img src="/nvidia-logo" alt="NVIDIA Logo" class="nvidia-logo-top">
        
        <div class="header">
            <h1>PRB Power dApp</h1>
            <p>Real-time Physical Resource Block Power & Inference Metrics</p>
        </div>
        
        <div class="status-bar">
            <span class="status-indicator" id="status-dot"></span>
            <span id="status-text">Connecting...</span>
        </div>
        
        <div class="plots-grid">
            <div class="plot-box">
                <h2>PRB Power Distribution</h2>
                <div id="prb-bars"></div>
            </div>
            
            <div class="plot-box">
                <h2>Inference Time</h2>
                <div id="client-timing-plot"></div>
            </div>
        </div>
        
        <div class="stats-container">
            <div class="stat-card">
                <h3>SFN / Slot</h3>
                <p id="stat-sfn-slot">- / -</p>
            </div>
            <div class="stat-card">
                <h3>C++ Client Time</h3>
                <p id="stat-total-time">-</p>
            </div>
            <div class="stat-card">
                <h3>Max PRB Power</h3>
                <p id="stat-max-power">-</p>
            </div>
            <div class="stat-card">
                <h3>Mean PRB Power</h3>
                <p id="stat-mean-power">-</p>
            </div>
            <div class="stat-card">
                <h3>Active PRBs</h3>
                <p id="stat-active-prbs">-</p>
            </div>
        </div>
        
        <div class="footer">
            <p>&copy; 2026 NVIDIA Corporation. All rights reserved.</p>
        </div>
    </div>

    <script>
        var STALE_THRESHOLD = 1.0;

        function setStatus(cls, text) {
            document.getElementById('status-dot').className = 'status-indicator ' + cls;
            document.getElementById('status-text').textContent = text;
        }

        function updateVisualizations() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    if (data.error) {
                        setStatus('status-waiting', 'Waiting for data...');
                        return;
                    }

                    var age = data.server_time - data.last_update;
                    if (age < STALE_THRESHOLD) {
                        setStatus('status-active', 'Live - Receiving Data');
                    } else {
                        setStatus('status-stale', 'Live - Waiting for Data');
                    }

                    updatePRBBars(data.prb_power);
                    updateClientTimingPlot(data.timing_history);
                    updateStats(data);
                })
                .catch(error => {
                    console.error('Fetch error:', error);
                    setStatus('status-error', 'Connection error');
                });
        }
        
        function updatePRBBars(prb_power) {
            // Convert to dB scale for better visualization
            let prb_power_db = prb_power.map(p => p > 0 ? 10 * Math.log10(p) : -100);
            
            let trace = {
                x: Array.from({length: prb_power.length}, (_, i) => i),
                y: prb_power_db,
                type: 'bar',
                marker: {
                    color: prb_power_db,
                    colorscale: 'Viridis',
                    showscale: false
                }
            };
            
            let layout = {
                title: '',
                xaxis: { 
                    title: { text: 'PRB Index', font: { size: 16 } },
                    range: [0, 273],
                    tickfont: { size: 14 }
                },
                yaxis: { 
                    title: { text: 'Power (dB)', font: { size: 16 } },
                    range: [-40, 30],
                    tickfont: { size: 14 }
                },
                height: 350,
                margin: { t: 10, b: 50, l: 60, r: 20 }
            };
            
            Plotly.newPlot('prb-bars', [trace], layout, {responsive: true});
        }
        
        function updateClientTimingPlot(timing_history) {
            if (!timing_history || timing_history.length === 0) {
                let layout = {
                    title: '',
                    xaxis: { 
                        title: { text: 'Sample Index', font: { size: 16 } },
                        tickfont: { size: 14 }
                    },
                    yaxis: { 
                        title: { text: 'Time (μs)', font: { size: 16 } },
                        range: [0, 3000],
                        tickfont: { size: 14 }
                    },
                    height: 350,
                    margin: { t: 10, b: 50, l: 60, r: 20 },
                    annotations: [{
                        text: 'Waiting for data...',
                        xref: 'paper',
                        yref: 'paper',
                        x: 0.5,
                        y: 0.5,
                        showarrow: false,
                        font: { size: 14, color: '#999' }
                    }]
                };
                Plotly.newPlot('client-timing-plot', [], layout, {responsive: true});
                return;
            }
            
            let times = Array.from({length: timing_history.length}, (_, i) => i);
            let client_times = timing_history.map(t => t.client);
            
            let trace = {
                x: times,
                y: client_times,
                type: 'scatter',
                mode: 'lines+markers',
                name: 'C++ Client Time',
                line: { color: '#76b900', width: 2 },
                marker: { size: 4 }
            };
            
            let layout = {
                title: '',
                xaxis: { 
                    title: { text: 'Sample Index', font: { size: 16 } },
                    tickfont: { size: 14 }
                },
                yaxis: { 
                    title: { text: 'Time (μs)', font: { size: 16 } },
                    range: [0, 3000],
                    tickfont: { size: 14 }
                },
                height: 350,
                margin: { t: 10, b: 50, l: 60, r: 20 }
            };
            
            Plotly.newPlot('client-timing-plot', [trace], layout, {responsive: true});
        }
        
        function updateStats(data) {
            // SFN / Slot
            if (data.sfn !== undefined && data.slot !== undefined) {
                document.getElementById('stat-sfn-slot').textContent = 
                    data.sfn + ' / ' + data.slot;
            }
            
            // C++ Client time
            if (data.timing && data.timing.client !== undefined) {
                document.getElementById('stat-total-time').innerHTML = 
                    data.timing.client.toFixed(1) + '<span class="unit">μs</span>';
            } else {
                document.getElementById('stat-total-time').innerHTML = 'N/A';
            }
            
            // Max PRB power (in dB)
            let max_power_linear = Math.max(...data.prb_power);
            let max_power_db = max_power_linear > 0 ? 10 * Math.log10(max_power_linear) : -100;
            document.getElementById('stat-max-power').innerHTML = 
                max_power_db.toFixed(1) + '<span class="unit">dB</span>';
            
            // Mean PRB power (in dB)
            let mean_power_linear = data.prb_power.reduce((a, b) => a + b, 0) / data.prb_power.length;
            let mean_power_db = mean_power_linear > 0 ? 10 * Math.log10(mean_power_linear) : -100;
            document.getElementById('stat-mean-power').innerHTML = 
                mean_power_db.toFixed(1) + '<span class="unit">dB</span>';
            
            // Active PRBs (power > 0 dB, i.e. linear > 1.0)
            let active_prbs = data.prb_power.filter(p => p > 1.0).length;
            document.getElementById('stat-active-prbs').textContent = active_prbs;
        }
        
        // Update every 100ms
        setInterval(updateVisualizations, 100);
        updateVisualizations();
    </script>
</body>
</html>
'''

def zmq_receiver(zmq_port):
    """Background thread to receive ZMQ data"""
    context = zmq.Context()
    socket = context.socket(zmq.SUB)
    socket.connect(f"tcp://localhost:{zmq_port}")
    socket.setsockopt_string(zmq.SUBSCRIBE, "")
    socket.setsockopt(zmq.RCVTIMEO, 1000)  # 1 second timeout
    
    print(f"ZMQ receiver connected to localhost:{zmq_port}")
    print("Waiting for PRB power data...")
    
    message_count = 0
    
    while True:
        try:
            # Receive metadata
            metadata = socket.recv_string()
            # Receive PRB power data
            prb_data = socket.recv(copy=False)
            # Receive timing data (optional)
            timing_data = None
            if socket.getsockopt(zmq.RCVMORE):
                timing_data = socket.recv(copy=False)
            
            message_count += 1
            
            # Parse metadata: "prb_power|sfn|slot|num_prbs"
            parts = metadata.split('|')
            
            if parts[0] == 'prb_power' and len(parts) >= 4:
                sfn = int(parts[1])
                slot = int(parts[2])
                num_prbs = int(parts[3])
                
                # Convert PRB power data to numpy array
                prb_power = np.frombuffer(prb_data.buffer, dtype=np.float32)
                
                if len(prb_power) != num_prbs:
                    print(f"[ERROR] Size mismatch! Expected {num_prbs}, got {len(prb_power)}")
                    continue
                
                # Parse timing data: [client_time, model_time]
                timing = None
                if timing_data:
                    timing_array = np.frombuffer(timing_data.buffer, dtype=np.float64)
                    if len(timing_array) >= 2:
                        timing = {
                            'client': timing_array[0],  # C++ client total time
                            'model': timing_array[1]    # Model inference time (-1 if N/A)
                        }
                
                # Calculate statistics
                max_power = float(np.max(prb_power))
                mean_power = float(np.mean(prb_power))
                max_idx = int(np.argmax(prb_power))
                
                # Convert to dB for console output
                max_power_db = 10 * np.log10(max_power) if max_power > 0 else -100
                mean_power_db = 10 * np.log10(mean_power) if mean_power > 0 else -100
                
                print(f"[PRB] SFN: {sfn}, Slot: {slot}, Max: {max_power_db:.1f} dB at PRB {max_idx}, Mean: {mean_power_db:.1f} dB")
                if timing:
                    client_str = f"Client: {timing['client']:.1f} μs"
                    model_str = f", Model: {timing['model']:.1f} μs" if timing['model'] >= 0 else ""
                    print(f"[TIMING] {client_str}{model_str}")
                
                # Update global state
                with data_lock:
                    global current_data, last_update_time
                    last_update_time = time.time()
                    current_data = {
                        'sfn': sfn,
                        'slot': slot,
                        'num_prbs': num_prbs,
                        'prb_power': prb_power.tolist(),
                        'max_power': max_power,
                        'mean_power': mean_power,
                        'max_idx': max_idx,
                        'timing': timing
                    }
                    
                    # Update history
                    prb_history.append(prb_power.tolist())
                    if timing:
                        timing_history.append(timing)
                        
            else:
                print(f"[ERROR] Invalid metadata format: {metadata}")
                    
        except zmq.Again:
            # Timeout - no data received
            continue
        except Exception as e:
            print(f"[ERROR] ZMQ receiver error: {e}")
            import traceback
            traceback.print_exc()

@app.route('/')
def index():
    return render_template_string(HTML_TEMPLATE)

@app.route('/nvidia-logo')
def nvidia_logo():
    """Serve the NVIDIA logo SVG"""
    logo_path = os.path.join(os.path.dirname(__file__), 'nvidia_logo.svg')
    if os.path.exists(logo_path):
        return send_file(logo_path, mimetype='image/svg+xml')
    else:
        # Fallback to embedded SVG if file not found
        return '''<svg viewBox="35.188 31.512 351.46 258.785" xmlns="http://www.w3.org/2000/svg">
            <path fill="#77B900" d="M82.211,102.414c0,0,22.504-33.203,67.437-36.638V53.73c-49.769,3.997-92.867,46.149-92.867,46.149s24.41,70.565,92.867,77.026v-12.804C99.411,157.781,82.211,102.414,82.211,102.414z M149.648,138.637v11.726c-37.968-6.769-48.507-46.237-48.507-46.237s18.23-20.195,48.507-23.47v12.867c-0.023,0-0.039-0.007-0.058-0.007c-15.891-1.907-28.305,12.938-28.305,12.938S128.243,131.445,149.648,138.637 M149.648,31.512V53.73c1.461-0.112,2.922-0.207,4.391-0.257c56.582-1.907,93.449,46.406,93.449,46.406s-42.343,51.488-86.457,51.488c-4.043,0-7.828-0.375-11.383-1.005v13.739c3.04,0.386,6.192,0.613,9.481,0.613c41.051,0,70.738-20.965,99.484-45.778c4.766,3.817,24.278,13.103,28.289,17.168c-27.332,22.883-91.031,41.329-127.144,41.329c-3.481,0-6.824-0.211-10.11-0.528v19.306h156.032V31.512H149.648z M149.648,80.656V65.777c1.446-0.101,2.903-0.179,4.391-0.226c40.688-1.278,67.382,34.965,67.382,34.965s-28.832,40.043-59.746,40.043c-4.449,0-8.438-0.715-12.028-1.922V93.523c15.84,1.914,19.028,8.911,28.551,24.786l21.18-17.859c0,0-15.461-20.277-41.524-20.277C155.021,80.172,152.31,80.371,149.648,80.656"/>
        </svg>''', 200, {'Content-Type': 'image/svg+xml'}

@app.route('/data')
def get_data():
    with data_lock:
        if current_data:
            response = current_data.copy()
            response['prb_history'] = list(prb_history)
            response['timing_history'] = list(timing_history)
            response['last_update'] = last_update_time
            response['server_time'] = time.time()
            return jsonify(response)
        else:
            return jsonify({'error': 'No data yet'})

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="PRB Power & Inference Time Visualizer")
    parser.add_argument('--port', type=int, default=5001, help='Web server port (default: 5001)')
    parser.add_argument('--zmq-port', type=int, default=5559, help='ZMQ subscriber port (default: 5559)')
    args = parser.parse_args()
    
    # Start ZMQ receiver thread
    receiver_thread = threading.Thread(target=zmq_receiver, args=(args.zmq_port,), daemon=True)
    receiver_thread.start()
    
    print(f"Starting web server on http://localhost:{args.port}")
    print("Open this URL in your browser to see the visualization")
    
    # Run Flask app
    app.run(host='0.0.0.0', port=args.port, debug=False)
