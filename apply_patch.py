import os
import json
import shutil
import subprocess
import sys


def apply_diff_blocks_to_file(original_lines: list[str], diffs) -> list[str]:
    """
    Apply the provided patch with represented by differences to their original lines.

    Parameters:
        original_lines (list[str]): The path to the backup folder.
        diffs (Any): The path to the backup folder.

    Returns:
        line (list[str]): The lines corresponding to the applied patch.
    """
    lines = original_lines[:]
    # Ensure diffs blocks are correctly formated in descending order to avoid line shifting issues
    diffs = sorted(diffs, key=lambda d: d['start_line'], reverse=True)

    for diff in diffs:
        idx = diff['start_line'] - 1

        if diff['type'] == 'deleted':
            original_block = [line + '\n' for line in diff['lines']]
            del lines[idx:idx + len(original_block)]

        elif diff['type'] == 'added':
            new_block = [line + '\n' for line in diff['lines']]
            lines[idx:idx] = new_block

        elif diff['type'] == 'modified':
            original_block = [line + '\n' for line in diff['original_lines']]
            new_block = [line + '\n' for line in diff['new_lines']]
            if lines[idx:idx + len(original_block)] == original_block:
                del lines[idx:idx + len(original_block)]
                lines[idx:idx] = new_block
            # Should not occur: original block not found, ignore it
            else:
                print(f"⚠️  The modified block cannot be found at the line {idx + 1} → ignored")
                continue

    return lines


def make_backup(backup_dir: str) -> None:
    """
    Run remove_patch.py if the provided backup path exists to restore the original state.
    After restoration (or if no backup existed), a brand new, empty backup directory
    is created, ready for the new patch files.

    Parameters:
        backup_dir (str): The path to the backup folder.
    """

    # Check if an existing backup needs to be restored
    if os.path.isdir(backup_dir):
        print(f"⚠️  Existing backup found → restoring files before patching...")

        try:
            # Execute the removal script
            subprocess.run(
                [sys.executable, "remove_patch.py"],
                check=True, # Raise a CalledProcessError if remove_patch.py fails
                capture_output=False
            )
            print("✅ Restoration successful.")

        except subprocess.CalledProcessError as e:
            # If the restoration fails, exit early
            print(f"❌ Error running remove_patch.py during restoration: {e}")
            print("Stderr:\n", e.stderr)
            sys.exit(1)
        except OSError as e:
            # If the backup couldn't be deleted, exit early
            print(f"❌ Critical error cleaning up old backup directory: {e}")
            sys.exit(1)

    # Create the brand new empty backup directory for the NEW patch
    os.makedirs(backup_dir, exist_ok=True)
    print(f"📁 Created new empty backup directory: {backup_dir}")


def apply_diff_directory(original_dir: str, diff_dir: str) -> None:
    """
    Apply the patch to the original folder.

    Parameters:
        original_dir (str): The path to the original folder.
        diff_dir (str): The path to the patch folder.
    """
    # Make a backup of the base files
    backup_dir = os.path.join(original_dir, "_backup_before_patch")
    make_backup(backup_dir)

    # Get a list of all json files of the patch
    diff_files = [f for f in os.listdir(diff_dir) if f.endswith('.json')]

    for diff_filename in diff_files:
        # Get paths to patch files and original files
        target_filename = diff_filename[:-5] # remove ".json" extension
        original_path = os.path.join(original_dir, target_filename)
        diff_path = os.path.join(diff_dir, diff_filename)

        # If the path to an original file does not exist, ignore it (should not occur)
        if not os.path.exists(original_path):
            print(f"❌ Missing original file: {target_filename} → ignored")
            continue

        # Get all lines from the original file and the patch file
        try:
            with open(original_path, 'r', encoding='utf-8') as f:
                original_lines = f.readlines()
            with open(diff_path, 'r', encoding='utf-8') as f:
                diffs = json.load(f)
        except Exception as e:
            print(f"❌ Error reading {target_filename}: {e}")
            continue

        # Virtually apply the patch to the lines from the original file
        patched_lines = apply_diff_blocks_to_file(original_lines, diffs)

        # Nothing to do if the patched lines and original lines are identical, skip it
        if patched_lines == original_lines:
            print(f"⏭️  Already patched or identical: {target_filename} → ignoré")
            continue

        # Make a backup of the original file before applying patch
        print(f"🛠️  Applying patch: {target_filename}")
        shutil.copy2(original_path, os.path.join(backup_dir, target_filename))
        # Apply the patched lines to the original file
        with open(original_path, 'w', encoding='utf-8') as f:
            f.writelines(patched_lines)
        print(f"✅ Patch applied with backup: {target_filename}")


apply_diff_directory(
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "mod/"
)
