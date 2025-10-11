import os
import json
import shutil

def apply_diff_blocks_to_file(original_lines, diffs):
    lines = original_lines[:]
    diffs = sorted(diffs, key=lambda d: d['start_line'], reverse=True)

    for diff in diffs:
        idx = diff['start_line'] - 1

        if diff['type'] == 'deleted':
            original_block = [line + '\n' for line in diff['lines']]
            # Nothing to delete
            if lines[idx:idx + len(original_block)] != original_block:
                continue
            del lines[idx:idx + len(original_block)]

        elif diff['type'] == 'added':
            new_block = [line + '\n' for line in diff['lines']]

            # Check if the block already exists somewhere
            found = False
            for i in range(len(lines) - len(new_block) + 1):
                if lines[i:i + len(new_block)] == new_block:
                    found = True
                    break

            # Ignore the existing block
            if found:
                continue

            # Otherwise, insert the block
            lines[idx:idx] = new_block


        elif diff['type'] == 'modified':
            original_block = [line + '\n' for line in diff['original_lines']]
            new_block = [line + '\n' for line in diff['new_lines']]
            # If already patched, nothing to do
            if lines[idx:idx + len(new_block)] == new_block:
                continue
            # Otherwise, replace if the original block is there
            if lines[idx:idx + len(original_block)] == original_block:
                del lines[idx:idx + len(original_block)]
                lines[idx:idx] = new_block
            else:
                # Original Block not found, ignore it
                print(f"⚠️ The modified block cannot be found at the line {idx + 1} → ignored")
                continue

    return lines

def apply_diff_directory(original_dir, diff_dir):
    backup_dir = os.path.join(original_dir, "_backup_before_patch")
    os.makedirs(backup_dir, exist_ok=True)

    diff_files = [f for f in os.listdir(diff_dir) if f.endswith('.json')]

    for diff_filename in diff_files:
        target_filename = diff_filename[:-5]
        original_path = os.path.join(original_dir, target_filename)
        diff_path = os.path.join(diff_dir, diff_filename)

        if not os.path.exists(original_path):
            print(f"❌ Missing original file: {target_filename} → ignored")
            continue

        try:
            with open(original_path, 'r', encoding='utf-8') as f:
                original_lines = f.readlines()
            with open(diff_path, 'r', encoding='utf-8') as f:
                diffs = json.load(f)
        except Exception as e:
            print(f"❌ Error reading {target_filename}: {e}")
            continue

        # Appliquer le patch virtuellement
        patched_lines = apply_diff_blocks_to_file(original_lines, diffs)

        if patched_lines == original_lines:
            print(f"⏭️  Already patched or identical: {target_filename} → ignoré")
            continue

        print(f"🛠️  Applying patch: {target_filename}")

        shutil.copy2(original_path, os.path.join(backup_dir, target_filename))
        with open(original_path, 'w', encoding='utf-8') as f:
            f.writelines(patched_lines)
        print(f"✅ Patch applied with backup: {target_filename}")

apply_diff_directory(
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "FA-SYMLINKER/mod/"
)
