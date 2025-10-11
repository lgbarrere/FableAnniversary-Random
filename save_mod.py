import os
import json
import difflib

# Authorized extensions of text files
TEXT_EXTENSIONS = {'.txt', '.py', '.json', '.xml', '.ini', '.cfg', '.tng'}

def is_text_file(filename):
    return os.path.splitext(filename)[1].lower() in TEXT_EXTENSIONS

def detect_diff_blocks(file1_path, file2_path):
    with open(file1_path, 'r', encoding='utf-8') as f1, open(file2_path, 'r', encoding='utf-8') as f2:
        lines1 = f1.readlines()
        lines2 = f2.readlines()

    sm = difflib.SequenceMatcher(None, lines1, lines2)
    diff_blocks = []

    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        elif tag == 'replace':
            diff_blocks.append({
                'type': 'modified',
                'start_line': i1 + 1,
                'original_lines': [line.rstrip('\n') for line in lines1[i1:i2]],
                'new_lines': [line.rstrip('\n') for line in lines2[j1:j2]]
            })
        elif tag == 'delete':
            diff_blocks.append({
                'type': 'deleted',
                'start_line': i1 + 1,
                'lines': [line.rstrip('\n') for line in lines1[i1:i2]]
            })
        elif tag == 'insert':
            diff_blocks.append({
                'type': 'added',
                'start_line': i1 + 1,
                'lines': [line.rstrip('\n') for line in lines2[j1:j2]]
            })

    return diff_blocks


def compare_directories(original_dir, modified_dir, output_diff_dir):
    os.makedirs(output_diff_dir, exist_ok=True)

    original_files = set(os.listdir(original_dir))
    modified_files = set(os.listdir(modified_dir))

    common_files = original_files & modified_files
    only_in_original = original_files - modified_files
    only_in_modified = modified_files - original_files

    if only_in_original:
        print("⚠️ Files only in the original folder:")
        for f in sorted(only_in_original):
            print(f"  - {f}")

    if only_in_modified:
        print("⚠️ Files only in the modified folder:")
        for f in sorted(only_in_modified):
            print(f"  - {f}")

    for filename in sorted(common_files):
        if not is_text_file(filename):
            continue

        path1 = os.path.join(original_dir, filename)
        path2 = os.path.join(modified_dir, filename)

        if not os.path.isfile(path1) or not os.path.isfile(path2):
            print(f"⏭️  Ignored (not a regular file): {filename}")
            continue

        try:
            with open(path1, 'r', encoding='utf-8') as f1, open(path2, 'r', encoding='utf-8') as f2:
                if f1.read() == f2.read():
                    continue
        except UnicodeDecodeError:
            print(f"❌ Unsupported Encoding in: {filename}")
            continue

        try:
            diffs = detect_diff_blocks(path1, path2)
        except UnicodeDecodeError:
            print(f"❌ The file could not be read in UTF-8: {filename}")
            continue

        if diffs:
            diff_output_path = os.path.join(output_diff_dir, filename + ".json")
            with open(diff_output_path, 'w', encoding='utf-8') as out:
                json.dump(diffs, out, indent=2)
            print(f"✅ Patch saved: {diff_output_path}")
        else:
            print(f"⚠️ Not difference found in: {filename}")

compare_directories(
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary - backup/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "mod/"
)
