import os
import subprocess
import threading
from collections import deque
from flask import Flask, jsonify, render_template

app = Flask(__name__)

process = None
log_lines = deque(maxlen=200)
log_lock = threading.Lock()


def _read_output(pipe):
    for line in iter(pipe.readline, ''):
        with log_lock:
            log_lines.append(line.rstrip())
    pipe.close()


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/start', methods=['POST'])
def start_airband():
    global process
    if process and process.poll() is None:
        return jsonify({'status': 'already_running'})

    cmd = ['rtl_airband', '-f', '-e', '-c', 'airband.conf']
    cwd = os.path.dirname(os.path.abspath(__file__))
    process = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    threading.Thread(target=_read_output, args=(process.stdout,), daemon=True).start()
    return jsonify({'status': 'started'})


@app.route('/stop', methods=['POST'])
def stop_airband():
    global process
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        return jsonify({'status': 'stopped'})
    return jsonify({'status': 'not_running'})


@app.route('/logs')
def get_logs():
    with log_lock:
        lines = list(log_lines)
    return jsonify({'lines': lines})


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
