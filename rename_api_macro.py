r"""
rename_api_macro.py

Recursively replaces PROJECTIDK_API with DEVSANDBOX_API in all .h/.cpp/.hpp files
under a given source directory (e.g. your Unreal project's Source folder).

Usage:
    python rename_api_macro.py "C:\_D\epic\unreal\projects\Migrate\Source"

If you don't pass an argument, it will default to the current directory.

By default this does a DRY RUN first (shows what would change, changes nothing).
Pass --apply to actually perform the replacement.

Examples:
    python rename_api_macro.py "C:\_D\epic\unreal\projects\Migrate\Source"          (dry run)
    python rename_api_macro.py "C:\_D\epic\unreal\projects\Migrate\Source" --apply  (actually edits files)
"""

import sys
import os

OLD = "PROJECTIDK_API"
NEW = "DEVSANDBOX_API"

# File extensions to search/edit
EXTENSIONS = {".h", ".hpp", ".cpp", ".cc", ".inl", ".cs"}


def find_target_files(root_dir):
    matches = []
    for dirpath, _, filenames in os.walk(root_dir):
        for fname in filenames:
            ext = os.path.splitext(fname)[1].lower()
            if ext in EXTENSIONS:
                matches.append(os.path.join(dirpath, fname))
    return matches


def process_file(filepath, apply_changes):
    try:
        with open(filepath, "r", encoding="utf-8-sig") as f:
            content = f.read()
    except (UnicodeDecodeError, PermissionError) as e:
        print(f"  [SKIPPED] {filepath} ({e})")
        return 0

    count = content.count(OLD)
    if count == 0:
        return 0

    print(f"  {filepath}  ->  {count} occurrence(s)")

    if apply_changes:
        new_content = content.replace(OLD, NEW)
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(new_content)

    return count


def main():
    args = sys.argv[1:]
    apply_changes = "--apply" in args
    args = [a for a in args if a != "--apply"]

    root_dir = args[0] if args else "."
    root_dir = os.path.abspath(root_dir)

    if not os.path.isdir(root_dir):
        print(f"Error: '{root_dir}' is not a valid directory.")
        sys.exit(1)

    mode = "APPLYING CHANGES" if apply_changes else "DRY RUN (no files will be modified)"
    print(f"Searching: {root_dir}")
    print(f"Mode: {mode}")
    print(f"Replacing: '{OLD}'  ->  '{NEW}'")
    print("-" * 60)

    files = find_target_files(root_dir)
    total_files_changed = 0
    total_occurrences = 0

    for filepath in files:
        count = process_file(filepath, apply_changes)
        if count > 0:
            total_files_changed += 1
            total_occurrences += count

    print("-" * 60)
    print(f"Files with matches: {total_files_changed}")
    print(f"Total occurrences:  {total_occurrences}")

    if not apply_changes and total_occurrences > 0:
        print("\nThis was a DRY RUN. Re-run with --apply to actually write changes:")
        print(f'    python rename_api_macro.py "{root_dir}" --apply')
    elif apply_changes:
        print("\nDone. Files have been updated.")
        print("Don't forget to also check your .Build.cs files and regenerate project files if needed.")


if __name__ == "__main__":
    main()
