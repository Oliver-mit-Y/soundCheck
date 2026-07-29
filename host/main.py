import subprocess


def run_command(command: str):
    """Execute a bash command string and return the completed process."""
    return subprocess.run(command, shell=True, text=True, check=True)


if __name__ == '__main__':
    run_command('echo "hello from soundCheck"')
