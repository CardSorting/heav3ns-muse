"""Verify native synthesis, project publication, rendering, and session restart."""
import argparse
import json
import queue
from pathlib import Path
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from studio_process_test import Studio


class RenderStudio(Studio):
    def __init__(self, binary, session, evidence):
        self.evidence = evidence
        super().__init__(binary, session, evidence)

    def receive(self, timeout=None):
        # Cold startup scans the native LADSPA modules through the OS loader.
        # Keep individual operations bounded independently of engine startup.
        if timeout is None:
            timeout = 90 if not hasattr(self, 'ready') else 30
        try:
            return super().receive(timeout=timeout)
        except queue.Empty:
            # Retain a native stack for intermittent macOS stalls before the
            # fixture's finally block terminates its own child process.
            if sys.platform == 'darwin' and self.process.poll() is None:
                try:
                    subprocess.run(
                        ['/usr/bin/sample', str(self.process.pid), '1', '1',
                         '-file', str(self.evidence / 'timeout.sample.txt')],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                        timeout=5, check=False,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    pass
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lmms', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve() / str(time.time_ns())
    root.mkdir(parents=True)
    binary = args.lmms.resolve()

    studios = []
    try:
        studio = RenderStudio(binary, root / 'session', root)
        studios.append(studio)
        for identifier, operation, arguments in [
            ('new', 'project.new', {'empty': True}),
            ('tempo', 'song.set', {'tempo': 120, 'masterVolume': 60}),
            ('track', 'track.add', {'type': 'instrument', 'name': 'Compile freeze verification'}),
            ('instrument', 'instrument.load', {'track': 0, 'instrument': 'tripleoscillator'}),
            ('clip', 'clip.add', {'track': 0, 'position': 0}),
        ]:
            studio.call(identifier, operation, arguments)
        for index, key in enumerate([60, 64, 67, 72]):
            studio.call('note-' + str(index), 'note.add', {'track': 0, 'clip': 0, 'position': index * 48, 'length': 48, 'key': key, 'volume': 65})
        project = root / 'composition.mmp'
        studio.call('save', 'project.save', {'path': str(project)})
        studio.call('load', 'project.load', {'path': str(project)})
        studio.call('tempo-after-load', 'song.set', {'tempo': 122})
        before = studio.call('inspect', 'project.inspect')['result']
        assert before['tempo'] == 122
        assert len(before['globalAutomations']) == 3
        assert all(not curve['hasAutomation'] and not curve['points'] for curve in before['globalAutomations'])
        rendered = studio.call('render', 'audio.render', {'path': str(root / 'composition.wav'), 'bitDepth': 16})['result']
        assert studio.call('after-render', 'project.inspect')['result']['tempo'] == 122
        measured = studio.call('analyze', 'audio.analyze', {'path': rendered['path'], 'sourceSha256': rendered['sha256']})['result']
        assert measured['sourceSha256'] == rendered['sha256']
        assert measured['nonfiniteSamples'] == 0 and measured['frames'] > 0
        assert any(channel['peak'] > 0.001 for channel in measured['channels']), measured
        studio.shutdown()
        restored = RenderStudio(binary, root / 'session', root)
        studios.append(restored)
        after = restored.call('reopened', 'project.inspect')['result']
        restored.shutdown()
        assert before['tracks'][0]['clips'][0]['notes'] == after['tracks'][0]['clips'][0]['notes']
        assert after['tempo'] == 122
        assert len(after['globalAutomations']) == 3
        assert all(not curve['hasAutomation'] and not curve['points'] for curve in after['globalAutomations'])
        (root / 'verification.json').write_text(json.dumps({'status': 'passed', 'render': rendered, 'analysis': measured, 'restoredNotes': after['tracks'][0]['clips'][0]['notes'], 'restoredTempo': after['tempo'], 'restoredGlobalAutomations': after['globalAutomations']}, indent=2))
        print(root)
    finally:
        for index, instance in enumerate(studios):
            if instance.process.poll() is None:
                instance.kill()
            (root / f'trace-{index}.json').write_text(json.dumps(instance.trace, indent=2))
            (root / f'stderr-{index}.log').write_text('\n'.join(instance.errors))


if __name__ == "__main__":
    main()
