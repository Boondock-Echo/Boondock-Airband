import os
import re
import subprocess
import threading
from collections import deque
from flask import Flask, jsonify, render_template, send_from_directory

app = Flask(__name__)

process = None
log_lines = deque(maxlen=200)
log_lock = threading.Lock()


def _read_output(pipe):
    for line in iter(pipe.readline, ''):
        with log_lock:
            log_lines.append(line.rstrip())
    pipe.close()


def _get_recordings_dir():
    """Return the first directory configured for file recordings.

    The rtl_airband configuration file may contain multiple `directory`
    entries. For the purposes of the web UI we only care about the first one
    found. If the configuration file cannot be read or no directory is found,
    ``None`` is returned.
    """

    config_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "airband.conf")
    try:
        with open(config_path, "r", encoding="utf-8") as cfg:
            match = re.search(r"directory\s*=\s*\"([^\"]+)\"", cfg.read())
            if match:
                return match.group(1)
    except FileNotFoundError:
        pass
    return None


@app.route('/')
def index():
    recordings_dir = _get_recordings_dir()
    recordings = []
    if recordings_dir and os.path.isdir(recordings_dir):
        for fname in sorted(os.listdir(recordings_dir)):
            if fname.lower().endswith((".mp3", ".wav", ".ogg")):
                recordings.append(fname)
    return render_template('index.html', recordings=recordings)


@app.route('/recordings/<path:filename>')
def serve_recording(filename):
    recordings_dir = _get_recordings_dir()
    if recordings_dir:
        return send_from_directory(recordings_dir, filename)
    return "Recording directory not configured", 404


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
