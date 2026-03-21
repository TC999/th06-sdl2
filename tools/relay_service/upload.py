#!/usr/bin/env python3
from __future__ import annotations

import argparse
import getpass
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Tuple


def ensure_paramiko():
    try:
        import paramiko  # type: ignore
    except ImportError:
        print("paramiko not found, installing it with pip...", flush=True)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "paramiko"])
        import paramiko  # type: ignore
    return paramiko


SCRIPT_DIR = Path(__file__).resolve().parent
FILES_TO_UPLOAD = (
    SCRIPT_DIR / "th06_relay_server.py",
    SCRIPT_DIR / "install.sh",
    SCRIPT_DIR / "README.md",
)


def ask(prompt: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{prompt}{suffix}: ").strip()
    return value or default


def quote_remote(value: str) -> str:
    return shlex.quote(value)


def sftp_mkdir_p(sftp, remote_dir: str) -> None:
    parts = []
    current = remote_dir
    while current not in ("", "/"):
        parts.append(current)
        current = os.path.dirname(current.rstrip("/"))
    for path in reversed(parts):
        try:
            sftp.stat(path)
        except OSError:
            sftp.mkdir(path)


def upload_files(sftp, remote_dir: str, files: Iterable[Path]) -> None:
    sftp_mkdir_p(sftp, remote_dir)
    for path in files:
        remote_path = f"{remote_dir.rstrip('/')}/{path.name}"
        print(f"Uploading {path.name} -> {remote_path}")
        sftp.put(str(path), remote_path)


def read_channel_output(stdout, stderr) -> Tuple[str, str]:
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    return out, err


def run_remote(ssh, command: str) -> int:
    print(f"\n[remote] {command}\n")
    stdin, stdout, stderr = ssh.exec_command(command)
    out, err = read_channel_output(stdout, stderr)
    if out:
        print(out, end="")
    if err:
        print(err, end="", file=sys.stderr)
    return stdout.channel.recv_exit_status()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Upload and deploy the TH06 relay service over SSH.")
    parser.add_argument("--host", help="SSH host or IP")
    parser.add_argument("--ssh-port", type=int, help="SSH port")
    parser.add_argument("--user", default="root", help="SSH username, defaults to root")
    parser.add_argument("--password", help="SSH password; if omitted, prompt securely")
    parser.add_argument("--remote-dir", default="/root/relay_service", help="Remote upload directory")
    parser.add_argument("--service-port", type=int, default=3478, help="Relay UDP port passed to install.sh")
    parser.add_argument("--log-level", default="INFO", help="TH06_RELAY_LOG_LEVEL for install.sh")
    parser.add_argument(
        "--traffic-mode",
        default="summary",
        choices=("summary", "packet", "off"),
        help="TH06_RELAY_TRAFFIC_MODE for install.sh",
    )
    parser.add_argument(
        "--traffic-interval",
        type=float,
        default=1.0,
        help="TH06_RELAY_TRAFFIC_INTERVAL for install.sh when using summary mode",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    host = args.host or ask("SSH IP")
    ssh_port = args.ssh_port or int(ask("SSH port", "22"))
    user = args.user or ask("SSH username", "root")
    password = args.password or getpass.getpass("SSH password: ")

    missing = [path.name for path in FILES_TO_UPLOAD if not path.is_file()]
    if missing:
        print(f"Missing local files: {', '.join(missing)}", file=sys.stderr)
        return 1

    paramiko = ensure_paramiko()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    print(f"Connecting to {user}@{host}:{ssh_port} ...")
    try:
        ssh.connect(
            hostname=host,
            port=ssh_port,
            username=user,
            password=password,
            look_for_keys=False,
            allow_agent=False,
            timeout=15,
        )
    except Exception as exc:
        print(f"SSH connect failed: {exc}", file=sys.stderr)
        return 1

    try:
        sftp = ssh.open_sftp()
        try:
            upload_files(sftp, args.remote_dir, FILES_TO_UPLOAD)
            remote_install = quote_remote(f"{args.remote_dir.rstrip('/')}/install.sh")
            remote_dir = quote_remote(args.remote_dir)
            remote_command = (
                f"cd {remote_dir} && chmod +x {remote_install} && "
                f"TH06_RELAY_LOG_LEVEL={quote_remote(args.log_level)} "
                f"TH06_RELAY_TRAFFIC_MODE={quote_remote(args.traffic_mode)} "
                f"TH06_RELAY_TRAFFIC_INTERVAL={quote_remote(str(args.traffic_interval))} "
                f"{remote_install} {args.service_port} && "
                f"systemctl restart th06-relay && "
                f"systemctl --no-pager --full status th06-relay"
            )
            status = run_remote(ssh, remote_command)
        finally:
            sftp.close()
    finally:
        ssh.close()

    if status != 0:
        print(f"\nRemote deployment failed with exit code {status}.", file=sys.stderr)
        return status

    print("\nDeployment finished.")
    print("Watch logs with: journalctl -u th06-relay -f -n 100 --no-pager")
    return 0


if __name__ == "__main__":
    sys.exit(main())
