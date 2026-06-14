import os
import json
import difflib

# Authorized extensions of text files
TEXT_EXTENSIONS = {'.txt', '.py', '.json', '.xml', '.ini', '.cfg', '.tng'}


def is_text_file(filename) -> bool:
    """
    Check if the provided file has an extension managed by this tool

    Parameters:
        filename (Any): The filename to check

    Returns:
        True is authorized, False otherwise
    """
    return os.path.splitext(filename)[1].lower() in TEXT_EXTENSIONS


def detect_diff_blocks(file1_path: str, file2_path: str) -> list:
    """
    Detect all differences between two provided files by block types:
        - added: New line(s) was/were added
        - deleted: Existing line(s) was/were deleted
        - modified: Existing line(s) was/were modified

    Parameters:
        file1_path (str): The path to the first file to compare
        file2_path (str): The path to the first file to compare

    Returns:
        diff_blocks (list): The list of strings representing the detected differences
    """
    # Read the files
    with open(file1_path, 'r', encoding='utf-8') as f1, open(file2_path, 'r', encoding='utf-8') as f2:
        lines1 = f1.readlines()
        lines2 = f2.readlines()

    # Get the differences through a sequence matcher
    sm = difflib.SequenceMatcher(None, lines1, lines2)
    diff_blocks = []

    # Fill up diff_blocks by tag type
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        # 'equal' means the lines are identical, skip it
        if tag == 'equal':
            continue
        # 'replace' means the lines were modified
        elif tag == 'replace':
            diff_blocks.append({
                'type': 'modified',
                'start_line': i1 + 1,
                'line_count': i2 - i1,
                'new_lines': [line.rstrip('\n') for line in lines2[j1:j2]]
            })
        # 'delete' means the lines were deleted
        elif tag == 'delete':
            diff_blocks.append({
                'type': 'deleted',
                'start_line': i1 + 1,
                'line_count': i2 - i1
            })
        # 'insert' means the lines were added
        elif tag == 'insert':
            diff_blocks.append({
                'type': 'added',
                'start_line': i1 + 1,
                'lines': [line.rstrip('\n') for line in lines2[j1:j2]]
            })

    return diff_blocks


def compare_directories(original_dir: str, modified_dir: str, output_diff_dir: str) -> None:
    """
    Compare all original files to their modified version and put the differences in a json

    Parameters:
        original_dir (str): The path to the folder containing the original files
        modified_dir (str): The path to the folder containing the modified files
        diff_dir (str): The path to the patch folder
    """
    # Create an output folder to save the patch
    os.makedirs(output_diff_dir, exist_ok=True)

    # Get all files from both original and modified folders
    original_files = set(os.listdir(original_dir))
    modified_files = set(os.listdir(modified_dir))

    # Ensure the same files names are identified in both the original and modified folders
    common_files = original_files & modified_files
    only_in_original = original_files - modified_files
    only_in_modified = modified_files - original_files

    if only_in_original:
        print("⚠️  Files only in the original folder:")
        for f in sorted(only_in_original):
            print(f"  - {f}")

    if only_in_modified:
        print("⚠️  Files only in the modified folder:")
        for f in sorted(only_in_modified):
            print(f"  - {f}")

    diff_found = False

    for filename in sorted(common_files):
        # Skip unauthorized files
        if not is_text_file(filename):
            continue

        path1 = os.path.join(original_dir, filename)
        path2 = os.path.join(modified_dir, filename)

        # If the path does not lead to a regular file, skip it
        if not os.path.isfile(path1) or not os.path.isfile(path2):
            print(f"⏭️  Ignored (not a regular file): {filename}")
            continue

        # Skip identical files
        try:
            with open(path1, 'r', encoding='utf-8') as f1, open(path2, 'r', encoding='utf-8') as f2:
                if f1.read() == f2.read():
                    continue
        except UnicodeDecodeError:
            print(f"❌ Unsupported Encoding in: {filename}")
            continue

        # Get all differences between the files
        try:
            diffs = detect_diff_blocks(path1, path2)
        except UnicodeDecodeError:
            print(f"❌ The file could not be read in UTF-8: {filename}")
            continue

        # Save the result in the output folder
        if diffs:
            diff_output_path = os.path.join(output_diff_dir, filename + ".json")
            with open(diff_output_path, 'w', encoding='utf-8') as out:
                json.dump(diffs, out, indent=2)
            print(f"✅ Patch saved: {diff_output_path}")
            diff_found = True
        else:
            # Should not occurr: diffs already identified earlier
            print(f"⚠️  No difference found in: {filename}")

    if diff_found == False:
        print(f"⚠️  The backup and Fable Anniversary are identical.")


compare_directories(
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary - backup/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/",
    "mod/"
)
