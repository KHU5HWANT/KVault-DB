import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import os

# Configuration
PROJECT_DIR = "."
OUTPUT_FILE = "project_context.md"

# Folders to completely ignore
IGNORE_DIRS = {
    ".git",
    "build",
    ".cache",
    "data",           # Database files (.sst, .wal)
    "node_modules",   # React dependencies
    "dist",           # React build output
    ".vite",          # Vite cache
    "_deps",          # CMake fetch content
    "CMakeFiles",     # CMake cache
    ".vscode",        # IDE settings
}

# File extensions to include (add or remove as needed)
INCLUDE_EXTENSIONS = {
    # C++ backend
    ".cpp", ".hpp", ".h", ".c",
    # React frontend
    ".js", ".jsx", ".css", ".html",
    # Build systems
    ".txt",  # For CMakeLists.txt
    ".json", # For package.json
    # Documentation
    ".md",
    # Configuration
    ".gitignore", ".py"
}

# Specific files to ignore even if they match extensions
IGNORE_FILES = {
    "package-lock.json",
    "CMakeCache.txt",
    OUTPUT_FILE,          # Don't include the output file in itself!
}

def is_text_file(filepath):
    """Basic check to see if a file extension is in our allowed list."""
    _, ext = os.path.splitext(filepath)
    # CMakeLists.txt and .gitignore don't always fit perfectly in ext, so we check basenames too
    basename = os.path.basename(filepath)
    if basename in {"CMakeLists.txt", ".gitignore", "package.json"}:
        return True
    return ext in INCLUDE_EXTENSIONS

def generate_context():
    print(f"🔍 Scanning project directory...")
    
    with open(OUTPUT_FILE, "w", encoding="utf-8") as out_file:
        out_file.write("# KVault-DB Project Context\n")
        out_file.write("This file contains the full source code of the KVault-DB project.\n\n")
        
        file_count = 0
        
        for root, dirs, files in os.walk(PROJECT_DIR):
            # Modify 'dirs' in-place to prevent os.walk from entering ignored directories
            dirs[:] = [d for d in dirs if d not in IGNORE_DIRS]
            
            for file in files:
                if file in IGNORE_FILES:
                    continue
                
                filepath = os.path.join(root, file)
                
                if is_text_file(filepath):
                    try:
                        # Try to read the file
                        with open(filepath, "r", encoding="utf-8") as in_file:
                            content = in_file.read()
                            
                        # Write to the output file clearly formatted
                        # We use relative paths for cleaner output
                        rel_path = os.path.relpath(filepath, PROJECT_DIR).replace("\\", "/")
                        out_file.write(f"\n## FILE: {rel_path}\n\n")
                        
                        # Add language identifier based on extension
                        basename = os.path.basename(filepath)
                        _, ext = os.path.splitext(filepath)
                        lang = ext[1:] if ext else ""
                        if basename == "CMakeLists.txt": lang = "cmake"
                        
                        out_file.write(f"```{lang}\n")
                        out_file.write(content)
                        if not content.endswith("\n"):
                            out_file.write("\n")
                        out_file.write("```\n\n")
                        
                        print(f"✅ Added: {rel_path}")
                        file_count += 1
                        
                    except UnicodeDecodeError:
                        print(f"⚠️ Skipped {filepath} (Not a valid UTF-8 text file)")
                    except Exception as e:
                        print(f"❌ Error reading {filepath}: {e}")
                        
    print(f"\n🎉 Done! Combined {file_count} files into '{OUTPUT_FILE}'.")
    print(f"You can now upload '{OUTPUT_FILE}' to Gemini Notebooks or any LLM.")

if __name__ == "__main__":
    generate_context()
