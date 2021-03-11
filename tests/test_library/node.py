from pathlib import Path
import subprocess

from node_config import NodeConfig


class Node:
    def __init__(self, name='unnamed', directory=Path()):
        self.name = name
        self.directory = directory
        self.executable_file_path = None
        self.process = None
        self.stdout_file = None
        self.stderr_file = None

        self.config = NodeConfig()
        self.config.add_entry(
            'log-appender',
            '{"appender":"stderr","stream":"std_error"} {"appender":"p2p","file":"logs/p2p/p2p.log"}',
            'Appender definition json: {"appender", "stream", "file"} Can only specify a file OR a stream'
        )

        self.config.add_entry(
            'log-logger',
            '{"name": "default", "level": "info", "appender": "stderr"} {"name": "p2p", "level": "warn", "appender": "p2p"}',
            'Logger definition json: {"name", "level", "appender"}'
        )

        self.config.add_entry(
            'backtrace',
            'yes',
            'Whether to print backtrace on SIGSEGV'
        )

        self.config.add_entry(
            'plugin',
            'witness account_by_key account_by_key_api condenser_api network_broadcast_api network_node_api',
            'Plugin(s) to enable, may be specified multiple times'
        )

        self.config.add_entry(
            'shared-file-dir',
            '"blockchain"',
            'The location of the chain shared memory files (absolute path or relative to application data dir)'
        )

        self.config.add_entry(
            'shared-file-size',
            '6G',
            'Size of the shared memory file. Default: 54G. If running a full node, increase this value to 200G'
        )

        self.config.add_entry(
            'enable-stale-production',
            '1',
            'Enable block production, even if the chain is stale'
        )

        self.config.add_entry(
            'required-participation',
            '0',
            'Percent of witnesses (0-99) that must be participating in order to produce blocks'
        )

    def get_p2p_endpoints(self):
        return self.config['p2p-endpoint']

    def run(self):
        if not self.executable_file_path:
            raise Exception('Missing executable')

        self.directory.mkdir()

        config_file_path = self.directory.joinpath('config.ini')
        self.config.write_to_file(config_file_path)

        self.stdout_file = open(self.directory/'stdout.txt', 'w')
        self.stderr_file = open(self.directory/'stderr.txt', 'w')

        self.process = subprocess.Popen(
            [
                self.executable_file_path,
                '--chain-id=04e8b5fc4bb4ab3c0ee3584199a2e584bfb2f141222b3a0d1c74e8a75ec8ff39',
                '-d', '.'
            ],
            cwd=self.directory,
            stdout=self.stdout_file,
            stderr=self.stderr_file
        )

        print(f'Node {self.name} run with pid {self.process.pid}')

    def close(self):
        self.process.kill()

    def wait_for_close(self):
        self.process.wait()

    def set_executable_file_path(self, executable_file_path):
        self.executable_file_path = executable_file_path

    def set_witness(self, witness, key):
        self.config.add_entry(
            'witness',
            f'"{witness}"',
            'Name of witness controlled by this node'
        )

        self.config.add_entry(
            'private-key',
            key
        )