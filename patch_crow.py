import os
import sys

def patch_crow():
    # Path is passed as the first argument
    if len(sys.argv) < 2:
        print("Usage: patch_crow.py <path_to_crow_include>")
        sys.exit(1)
        
    crow_include_dir = sys.argv[1]
    routing_h_path = os.path.join(crow_include_dir, "crow", "routing.h")
    
    if not os.path.exists(routing_h_path):
        print(f"Error: {routing_h_path} not found.")
        sys.exit(1)
        
    with open(routing_h_path, "r", encoding="utf-8") as f:
        content = f.read()
        
    # Replace the hardcoded OPTIONS handling condition to effectively disable it
    # This fixes a bug in Crow where intercepting OPTIONS requests and subsequently 
    # attempting to modify the response (e.g. for CORS and setting 200 OK) causes 
    # double complete_request calls and corrupts the async_write buffers.
    target_str = "else if (req.method == HTTPMethod::Options)"
    replacement_str = "else if (false && req.method == HTTPMethod::Options)"
    
    if target_str in content:
        content = content.replace(target_str, replacement_str)
        with open(routing_h_path, "w", encoding="utf-8") as f:
            f.write(content)
        print("Successfully patched crow/routing.h to bypass internal OPTIONS handling.")
    elif replacement_str in content:
        print("crow/routing.h is already patched.")
    else:
        print("Warning: Could not find the target string in crow/routing.h. The file might have been updated.")

if __name__ == "__main__":
    patch_crow()
