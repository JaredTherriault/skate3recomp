#!/usr/bin/env python3
"""One-click release automation for Skate 3 Recomp.

Compiles local binaries, packages clean retail-free release zips for Windows and Linux,
tags the git release, and uploads both archives to GitHub using the GitHub CLI (gh).

Usage:
    python3 tools/publish_release.py [TAG] [--notes "Release notes"] [--dry-run]

Example:
    python3 tools/publish_release.py v2.0.3 --notes "Updated custom OTS challenges"
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / 'build'
DIST_DIR = REPO_ROOT / 'dist'
WINDOWS_BIN_DIR = REPO_ROOT / 'bin' / 'windows'


def get_target_repo():
    """Detect the GitHub repository for remote.origin."""
    url = run_cmd(['git', 'config', '--get', 'remote.origin.url'], check=False).stdout.strip()
    m = re.search(r'github\.com[:/]([^/]+/[^/.]+)', url)
    if m:
        repo = m.group(1)
        if repo.endswith('.git'):
            repo = repo[:-4]
        return repo
    return 'JaredTherriault/skate3recomp'


def run_cmd(cmd, cwd=REPO_ROOT, check=True):
    """Run a shell command and return the completed process."""
    print(f"-> {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if check and result.returncode != 0:
        print(f"Error executing command: {' '.join(str(c) for c in cmd)}", file=sys.stderr)
        if result.stdout:
            print(f"STDOUT:\n{result.stdout}", file=sys.stderr)
        if result.stderr:
            print(f"STDERR:\n{result.stderr}", file=sys.stderr)
        sys.exit(result.returncode)
    return result


def check_gh_cli():
    """Verify GitHub CLI is installed and authenticated."""
    gh_path = shutil.which('gh')
    if not gh_path:
        print("Error: GitHub CLI (gh) is not found in PATH.", file=sys.stderr)
        print("Please install gh or ensure ~/.local/bin is in your PATH.", file=sys.stderr)
        sys.exit(1)

    auth = subprocess.run(['gh', 'auth', 'status'], text=True, capture_output=True)
    if auth.returncode != 0:
        print("\nGitHub CLI is not logged in. Please run:", file=sys.stderr)
        print("    gh auth login", file=sys.stderr)
        print("to authenticate with your GitHub account, then rerun this script.\n", file=sys.stderr)
        sys.exit(1)


def build_linux():
    """Build the Linux executable and runtime library."""
    print("\n=== Building Linux Binaries ===")
    if not (BUILD_DIR / 'build.ninja').exists():
        print("Configuring CMake for Linux...")
        run_cmd(['cmake', '-B', str(BUILD_DIR), '-GNinja', '-DCMAKE_BUILD_TYPE=Release'])
    run_cmd(['ninja', '-C', str(BUILD_DIR), 'skate3', 'rexruntime'])


def package_clean_zip(zip_path, folder_name, files):
    """Create a clean zip archive with files placed under folder_name/."""
    print(f"Packaging {zip_path.name}...")
    if zip_path.exists():
        zip_path.unlink()

    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for src_path, target_rel in files:
            if not src_path.exists():
                print(f"Error: Required file {src_path} is missing!", file=sys.stderr)
                sys.exit(1)
            archive_name = f"{folder_name}/{target_rel}"
            zf.write(src_path, archive_name)

    size_mb = zip_path.stat().st_size / (1024 * 1024)
    print(f"  Created {zip_path.name} ({size_mb:.1f} MB)")


def main():
    parser = argparse.ArgumentParser(description="Build and publish Skate 3 Recomp releases to GitHub.")
    parser.add_argument('tag', nargs='?', help="Release tag (e.g. v2.0.3)")
    parser.add_argument('--title', help="Release title (defaults to tag)")
    parser.add_argument('--notes', help="Release description / notes")
    parser.add_argument('--skip-build', action='store_true', help="Skip rebuilding Linux binaries")
    parser.add_argument('--dry-run', action='store_true', help="Build and package without pushing tag or release")
    args = parser.parse_args()

    # Determine tag
    tag = args.tag
    if not tag:
        # Check current latest git tag
        latest_tag = run_cmd(['git', 'describe', '--tags', '--abbrev=0'], check=False).stdout.strip()
        print(f"Latest git tag: {latest_tag if latest_tag else 'None'}")
        tag = input("Enter new release tag (e.g. v2.0.3): ").strip()
        if not tag:
            print("Tag is required.")
            sys.exit(1)

    if not tag.startswith('v'):
        tag = f"v{tag}"

    title = args.title or tag
    notes = args.notes or f"Release {tag} for Skate 3 Recompiled"

    if not args.dry_run:
        check_gh_cli()

    # Step 1: Build
    if not args.skip_build:
        build_linux()

    # Step 2: Ensure distribution directory exists
    DIST_DIR.mkdir(exist_ok=True)

    # Step 3: Package Linux Archive (Skate3Recomp-Linux.zip)
    linux_zip = DIST_DIR / 'Skate3Recomp-Linux.zip'
    linux_files = [
        (BUILD_DIR / 'skate3', 'skate3'),
        (BUILD_DIR / 'librexruntime.so', 'librexruntime.so'),
    ]
    package_clean_zip(linux_zip, 'Skate3Recomp-Linux', linux_files)

    # Step 4: Package Windows Archive (Skate3Recomp-Windows.zip)
    win_zip = DIST_DIR / 'Skate3Recomp-Windows.zip'
    if not (WINDOWS_BIN_DIR / 'skate3.exe').exists() or not (WINDOWS_BIN_DIR / 'rexruntime.dll').exists():
        print(f"Error: Missing Windows binaries in {WINDOWS_BIN_DIR}.", file=sys.stderr)
        print("Please ensure skate3.exe and rexruntime.dll exist in bin/windows/.", file=sys.stderr)
        sys.exit(1)

    win_files = [
        (WINDOWS_BIN_DIR / 'skate3.exe', 'skate3.exe'),
        (WINDOWS_BIN_DIR / 'rexruntime.dll', 'rexruntime.dll'),
    ]
    package_clean_zip(win_zip, 'Skate3Recomp-Windows', win_files)

    print("\nClean, retail-free release archives ready in dist/:")
    print(f"  - {linux_zip.name} ({linux_zip.stat().st_size / (1024*1024):.1f} MB)")
    print(f"  - {win_zip.name} ({win_zip.stat().st_size / (1024*1024):.1f} MB)")

    if args.dry_run:
        print("\n[Dry Run] Skipping git tag and GitHub release creation.")
        return

    # Step 5: Create and push git tag
    print(f"\nCreating git tag '{tag}'...")
    run_cmd(['git', 'tag', '-f', tag])
    print(f"Pushing tag '{tag}' to origin...")
    run_cmd(['git', 'push', 'origin', tag, '--force'])

    target_repo = get_target_repo()
    print(f"Target GitHub repository: {target_repo}")

    # Step 6: Create or update GitHub Release
    print(f"\nPublishing release '{tag}' to GitHub via gh...")
    # Check if release exists
    check_release = run_cmd(['gh', 'release', 'view', tag, '-R', target_repo], check=False)
    if check_release.returncode == 0:
        print(f"Release {tag} already exists on GitHub. Uploading updated assets...")
        run_cmd([
            'gh', 'release', 'upload', tag,
            str(linux_zip),
            str(win_zip),
            '-R', target_repo,
            '--clobber'
        ])
    else:
        print(f"Creating new GitHub release {tag}...")
        run_cmd([
            'gh', 'release', 'create', tag,
            str(linux_zip),
            str(win_zip),
            '-R', target_repo,
            '--title', title,
            '--notes', notes
        ])

    view_url = run_cmd(['gh', 'release', 'view', tag, '-R', target_repo, '--json', 'url', '-q', '.url'], check=False).stdout.strip()
    print("\n=======================================================")
    print("SUCCESS! Release published successfully:")
    if view_url:
        print(f"Release URL: {view_url}")
    print("Quiver Launcher on your Steam Deck will now detect this version!")
    print("=======================================================")


if __name__ == '__main__':
    main()
