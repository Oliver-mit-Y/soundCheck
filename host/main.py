import subprocess
import yaml


def run_command(command: str):
    """Execute a bash command string and return the completed process."""
    return subprocess.run(command, shell=True, text=True, check=True)


if __name__ == '__main__':
    with open("env", "r") as f:
        data = yaml.safe_load(f)

    print(data)
    
    print(run_command(f"export SPOTIPY_CLIENT_ID='{data['client_id']}' ; export SPOTIPY_CLIENT_SECRET='{data['client_secret']}' ; export SPOTIPY_REDIRECT_URI='example.com'"))
