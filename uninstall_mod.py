import os
import shutil

def unpatch_directory(original_dir):
    backup_dir = os.path.join(original_dir, "_backup_before_patch")

    if not os.path.exists(backup_dir):
        print(f"❌ Backup not found: {backup_dir}")
        return

    backup_files = [f for f in os.listdir(backup_dir) if os.path.isfile(os.path.join(backup_dir, f))]
    if not backup_files:
        print("📁 The backup is empty: {backup_dir}")
        return

    print(f"🔄 Restoring from: {backup_dir}")

    restored_count = 0

    for file in backup_files:
        backup_path = os.path.join(backup_dir, file)
        original_path = os.path.join(original_dir, file)

        try:
            shutil.copy2(backup_path, original_path)
            print(f"✅ Restored: {file}")
            restored_count += 1
        except Exception as e:
            print(f"❌ Error while restoring {file}: {e}")

    print(f"🎉 {restored_count} file(s) restored")

    try:
        shutil.rmtree(backup_dir)
        print(f"🗑️  Backup removed: {backup_dir}")
    except Exception as e:
        print(f"❌ Error while removing the backup: {e}")

unpatch_directory(
    "D:/Program Files (x86)/Steam/steamapps/common/Fable Anniversary/WellingtonGame/FableData/Build/Data/Levels/FinalAlbion/"
)
